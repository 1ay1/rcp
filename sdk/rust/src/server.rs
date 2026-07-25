//! server.rs — expose a Rust RAG engine as an RCP/1 server.
//!
//! Mirrors the C++/Python/Node `Server`: it owns the JSON-RPC framing, the
//! `initialize` handshake, capability gating, batching, and error mapping; you
//! register handlers with `on` and identity/capabilities with `set_info` /
//! `advertise`. Handlers are `FnMut(&Json) -> Result<Json, RcpError>`.

use crate::json::{obj, Json};
use crate::types::{
    cap_for_method, negotiate_version, Capability, Errc, Method, RcpError, MIN_PROTOCOL_VERSION,
    PROTOCOL_VERSION,
};
use std::collections::HashMap;
use std::io::{BufRead, BufReader, Read, Write};
use std::net::{TcpListener, TcpStream};

type Handler = Box<dyn FnMut(&Json) -> Result<Json, RcpError>>;

/// An RCP/1 server.
pub struct Server {
    info: Json,
    caps: Json,
    handlers: HashMap<String, Handler>,
    initialized: bool,
    /// Protocol version agreed at the handshake (§7.1).
    negotiated: i64,
}

impl Default for Server {
    fn default() -> Server {
        Server::new()
    }
}

/// Count-like params that share one rule across every method (spec §4.6/§7.7): a
/// value out of RANGE is clamped by the server, but a value of the wrong TYPE or
/// sign is structurally invalid and MUST be -32602. Validating centrally means no
/// handler can forget, and all four SDKs reject the same inputs identically.
const POSITIVE_INT_FIELDS: &[&str] =
    &["k", "topN", "candidateK", "n", "hops", "tokenBudget", "limit"];

/// Returns `Some(RcpError)` when `params` is structurally invalid, else `None`.
/// Enforces only method-independent invariants the spec states as MUST;
/// semantic checks stay in the handler.
fn validate_params(params: &Json) -> Option<RcpError> {
    if params.as_object().is_none() {
        return Some(RcpError::new(Errc::INVALID_PARAMS, "'params' MUST be an object"));
    }
    let int_of = |v: &Json| -> Option<i64> {
        if v.as_bool().is_some() {
            return None; // a JSON bool is not an integer
        }
        let n = v.as_f64()?;
        if n.fract() == 0.0 { Some(n as i64) } else { None }
    };

    for field in POSITIVE_INT_FIELDS {
        let Some(v) = params.get(*field) else { continue };
        match int_of(v) {
            None => {
                return Some(RcpError::with_data(
                    Errc::INVALID_PARAMS,
                    format!("'{field}' MUST be an integer"),
                    obj(&[("field", Json::from(*field))]),
                ))
            }
            Some(n) if n < 1 => {
                return Some(RcpError::with_data(
                    Errc::INVALID_PARAMS,
                    format!("'{field}' MUST be >= 1"),
                    obj(&[("field", Json::from(*field))]),
                ))
            }
            Some(_) => {}
        }
    }

    // §3.3 funnel invariant: candidateK >= rerank.topN >= k.
    let k = params.get("k").and_then(int_of);
    let cand = params.get("candidateK").and_then(int_of);
    let top_n = params.get("rerank").and_then(|r| r.get("topN")).and_then(int_of);
    let viol = |field: &str, msg: &str| {
        Some(RcpError::with_data(
            Errc::INVALID_PARAMS,
            msg.to_string(),
            obj(&[("field", Json::from(field))]),
        ))
    };
    if let (Some(k), Some(c)) = (k, cand) {
        if c < k {
            return viol("candidateK", "'candidateK' MUST be >= 'k' (funnel invariant)");
        }
    }
    if let (Some(k), Some(t)) = (k, top_n) {
        if t < k {
            return viol("rerank.topN", "'rerank.topN' MUST be >= 'k' (funnel invariant)");
        }
    }
    if let (Some(c), Some(t)) = (cand, top_n) {
        if c < t {
            return viol(
                "candidateK",
                "'candidateK' MUST be >= 'rerank.topN' (funnel invariant)",
            );
        }
    }
    None
}

impl Server {
    pub fn new() -> Server {
        Server {
            info: obj(&[("name", "unknown".into()), ("version", "0".into())]),
            caps: Json::object(),
            handlers: HashMap::new(),
            initialized: false,
            negotiated: PROTOCOL_VERSION,
        }
    }

    // ── configuration ───────────────────────────────────────────────────────
    pub fn set_info(&mut self, name: &str, version: &str) -> &mut Self {
        self.info = obj(&[("name", name.into()), ("version", version.into())]);
        self
    }

    /// Advertise a capability with optional metadata (pass `Json::object()` for
    /// none). Advertising is independent of registering a handler; a gated call
    /// needs both or it is CapabilityMissing.
    pub fn advertise(&mut self, cap: Capability, meta: Json) -> &mut Self {
        self.caps.insert(cap.wire_key(), meta);
        self
    }

    /// Register a handler for `method` (a `Method::*` constant or its string).
    pub fn on<F>(&mut self, method: &str, handler: F) -> &mut Self
    where
        F: FnMut(&Json) -> Result<Json, RcpError> + 'static,
    {
        self.handlers.insert(method.to_string(), Box::new(handler));
        self
    }

    // ── dispatch ──────────────────────────────────────────────────────────────
    /// Handle one JSON-RPC request → reply, or `None` for a notification that
    /// warrants no response. Never panics.
    pub fn handle(&mut self, request: &Json) -> Option<Json> {
        let id = request.get("id").cloned().unwrap_or(Json::Null);
        let method = match request.get("method").and_then(|m| m.as_str()) {
            Some(m) => m.to_string(),
            None => return Some(err(&id, Errc::INVALID_REQUEST, "missing 'method'")),
        };
        let params = request.get("params").cloned().unwrap_or_else(Json::object);

        // §4.5: a message with NO `id` is a notification and MUST NOT be
        // answered — whatever it asks for, known method or not. This check must
        // precede all dispatch: answering a notification (even with an error)
        // desynchronises a pipelining client, which correlates strictly by id.
        if request.get("id").is_none() {
            return None;
        }

        match method.as_str() {
            Method::INITIALIZE => {
                // §13: after a SUCCESSFUL handshake a second `initialize` MUST
                // be rejected with -32600 — re-negotiating mid-session would
                // silently invalidate every capability the client has cached. A
                // repeat BEFORE any success (retry after -32002) is allowed.
                if self.initialized {
                    return Some(err(
                        &id,
                        Errc::INVALID_REQUEST,
                        "already initialized; re-negotiation is not permitted mid-session",
                    ));
                }
                let peer = params
                    .get("protocolVersion")
                    .and_then(|v| v.as_i64())
                    .unwrap_or(PROTOCOL_VERSION);
                let neg = negotiate_version(peer);
                if neg < MIN_PROTOCOL_VERSION {
                    return Some(err(&id, Errc::VERSION_MISMATCH, "no common version"));
                }
                // §7.1: the session is initialized only on a SUCCESSFUL
                // handshake. Setting this before the version check would leave a
                // server that just rejected the peer's version fully unlocked.
                self.initialized = true;
                self.negotiated = neg;
                Some(ok(&id, self.info_result(neg)))
            }
            Method::INFO => Some(ok(&id, self.info_result(PROTOCOL_VERSION))),
            Method::PING => {
                let result = match params.get("nonce") {
                    Some(n) => obj(&[("nonce", n.clone())]),
                    None => Json::object(),
                };
                Some(ok(&id, result))
            }
            // `notifications/cancel` carrying an `id` is malformed: §7.14
            // defines it as a notification only. Reject rather than accept.
            Method::CANCEL => Some(err(
                &id,
                Errc::INVALID_REQUEST,
                "'notifications/cancel' is a notification and MUST NOT carry an 'id'",
            )),
            // §7.12/§13: shutdown is always available. It is the graceful twin
            // of EOF-on-stdin (which is unconditional), leaks nothing, and
            // refusing it would force a peer to handshake pointlessly or kill.
            Method::SHUTDOWN => Some(ok(&id, Json::object())),
            _ => {
                if !self.initialized {
                    return Some(err(&id, Errc::NOT_INITIALIZED, "call 'initialize' first"));
                }
                Some(self.dispatch(&id, &method, &params))
            }
        }
    }

    fn dispatch(&mut self, id: &Json, method: &str, params: &Json) -> Json {
        let cap = match cap_for_method(method) {
            Some(c) => c,
            None => return err(id, Errc::UNKNOWN_METHOD, &format!("unknown method '{}'", method)),
        };
        let advertised = self.caps.get(cap.wire_key()).is_some_and(|v| !v.is_null());
        if !self.handlers.contains_key(method) {
            return err(id, Errc::CAPABILITY_MISSING, &format!("'{}' not implemented", method));
        }
        if !advertised {
            return err(
                id,
                Errc::CAPABILITY_MISSING,
                &format!("capability '{}' not supported", cap.wire_key()),
            );
        }
        // §12: params are validated only after capability gating, so a client
        // learns "I don't do that at all" before "your argument was malformed".
        if let Some(e) = validate_params(params) {
            return err_data(id, e.code, &e.message, e.data);
        }
        let handler = self.handlers.get_mut(method).expect("handler present");
        match handler(params) {
            Ok(result) => ok(id, result),
            Err(e) => err_data(id, e.code, &e.message, e.data),
        }
    }

    /// Parse a JSON line, dispatch (single or batch), and return the reply
    /// string ("" when there is no response, e.g. an all-notification batch).
    pub fn handle_line(&mut self, line: &str) -> String {
        let msg = match Json::parse(line) {
            Ok(m) => m,
            Err(e) => return err(&Json::Null, Errc::PARSE_ERROR, &e).dump(),
        };
        if let Json::Array(items) = &msg {
            if items.is_empty() {
                return err(&Json::Null, Errc::INVALID_REQUEST, "empty batch").dump();
            }
            let mut out = Vec::new();
            for item in items {
                if let Some(reply) = self.handle(item) {
                    out.push(reply);
                }
            }
            return if out.is_empty() {
                String::new()
            } else {
                Json::Array(out).dump()
            };
        }
        match self.handle(&msg) {
            Some(reply) => reply.dump(),
            None => String::new(),
        }
    }

    fn info_result(&self, version: i64) -> Json {
        obj(&[
            ("protocolVersion", version.into()),
            ("server", self.info.clone()),
            ("capabilities", self.caps.clone()),
        ])
    }

    // ── serving ──────────────────────────────────────────────────────────────
    /// Serve newline-delimited JSON-RPC over stdin/stdout until shutdown/EOF.
    pub fn serve_stdio(&mut self) {
        let stdin = std::io::stdin();
        let mut reader = stdin.lock();
        let stdout = std::io::stdout();
        let mut out = stdout.lock();
        let mut line = String::new();
        loop {
            line.clear();
            match reader.read_line(&mut line) {
                Ok(0) | Err(_) => break, // EOF or error
                Ok(_) => {}
            }
            let trimmed = line.trim();
            if trimmed.is_empty() {
                continue;
            }
            let is_shutdown = Json::parse(trimmed)
                .ok()
                .and_then(|m| m.get("method").and_then(|x| x.as_str()).map(|s| s == Method::SHUTDOWN))
                .unwrap_or(false);
            let reply = self.handle_line(trimmed);
            if !reply.is_empty()
                && out
                    .write_all(reply.as_bytes())
                    .and_then(|_| out.write_all(b"\n"))
                    .and_then(|_| out.flush())
                    .is_err()
            {
                break; // peer gone
            }
            if is_shutdown {
                break;
            }
        }
    }

    /// Serve JSON-RPC over minimal HTTP/1.1 on `127.0.0.1:port` (200 for a
    /// response, 204 for a notification). Loops until an accept error.
    pub fn serve_http(&mut self, port: u16) -> Result<(), RcpError> {
        let listener = TcpListener::bind(("127.0.0.1", port))
            .map_err(|e| RcpError::new(Errc::BACKEND_UNAVAILABLE, format!("bind failed: {}", e)))?;
        for stream in listener.incoming() {
            match stream {
                Ok(mut s) => {
                    let _ = self.serve_http_conn(&mut s);
                }
                Err(_) => continue,
            }
        }
        Ok(())
    }

    fn serve_http_conn(&mut self, stream: &mut TcpStream) -> std::io::Result<()> {
        let mut reader = BufReader::new(stream.try_clone()?);
        let mut content_length = 0usize;
        loop {
            let mut header = String::new();
            let n = reader.read_line(&mut header)?;
            if n == 0 {
                return Ok(());
            }
            if header == "\r\n" || header == "\n" {
                break; // end of headers
            }
            let lower = header.to_ascii_lowercase();
            if let Some(rest) = lower.strip_prefix("content-length:") {
                content_length = rest.trim().parse().unwrap_or(0);
            }
        }
        let mut body = vec![0u8; content_length];
        reader.read_exact(&mut body)?;
        let text = String::from_utf8_lossy(&body);
        let text = if text.trim().is_empty() { "{}" } else { text.trim() };
        let reply = self.handle_line(text);
        let response = if reply.is_empty() {
            "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n".to_string()
        } else {
            format!(
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\
                 Content-Length: {}\r\nConnection: close\r\n\r\n{}",
                reply.len(),
                reply
            )
        };
        stream.write_all(response.as_bytes())?;
        stream.flush()
    }
}

// ── notification builders ────────────────────────────────────────────────────
/// Build a `notifications/log` frame (spec §17.1) to write to the output stream.
pub fn make_log_notification(level: &str, message: &str, data: Option<Json>) -> String {
    let mut params = obj(&[("level", level.into()), ("message", message.into())]);
    if let Some(d) = data {
        params.insert("data", d);
    }
    obj(&[
        ("jsonrpc", "2.0".into()),
        ("method", Method::LOG.into()),
        ("params", params),
    ])
    .dump()
}

/// Build a `notifications/progress` frame (spec §9) during a long retrieve.
pub fn make_progress_notification(
    token: Json,
    progress: f64,
    stage: Option<&str>,
    partial: Option<Json>,
) -> String {
    let mut params = obj(&[("progressToken", token), ("progress", progress.into())]);
    if let Some(s) = stage {
        if !s.is_empty() {
            params.insert("stage", s);
        }
    }
    if let Some(p) = partial {
        params.insert("partial", p);
    }
    obj(&[
        ("jsonrpc", "2.0".into()),
        ("method", Method::PROGRESS.into()),
        ("params", params),
    ])
    .dump()
}

// ── envelope helpers ─────────────────────────────────────────────────────────
fn ok(id: &Json, result: Json) -> Json {
    obj(&[
        ("jsonrpc", "2.0".into()),
        ("id", id.clone()),
        ("result", result),
    ])
}

fn err(id: &Json, code: i64, message: &str) -> Json {
    err_data(id, code, message, None)
}

fn err_data(id: &Json, code: i64, message: &str, data: Option<Json>) -> Json {
    let mut error = obj(&[("code", code.into()), ("message", message.into())]);
    if let Some(d) = data {
        error.insert("data", d);
    }
    obj(&[
        ("jsonrpc", "2.0".into()),
        ("id", id.clone()),
        ("error", error),
    ])
}
