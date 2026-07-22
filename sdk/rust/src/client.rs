//! client.rs — connect to any RCP server (subprocess or HTTP), then typed calls.
//!
//! Mirrors the C++/Python/Node `Client`: connecting performs the `initialize`
//! handshake and caches the negotiated protocol version, server identity, and
//! capabilities. Every typed call is **capability-gated** — a call to a feature
//! the server never advertised returns `Err(CapabilityMissing)` before any
//! round-trip. Synchronous and blocking; every call returns `Result<_, RcpError>`.

use crate::json::{obj, Json};
use crate::transport::{HttpTransport, StdioTransport, Transport};
use crate::types::{
    Capability, Errc, Method, RcpError, MIN_PROTOCOL_VERSION, PROTOCOL_VERSION,
};

/// One retrieval hit. `id`/`score`/`text` are always present (id coerced to a
/// string); every optional spec §7.7 field is surfaced when the server sent it —
/// `confidence` (normalised [0,1]), `unit`/`level` (granularity & abstraction),
/// `provenance` (graph/tree lineage), `trust` (provenance + safety), and
/// per-stage `scores`. `raw` is the untouched JSON object for anything else.
#[derive(Clone, Debug)]
pub struct Hit {
    pub id: String,
    pub score: f64,
    pub text: String,
    pub citation: Option<Json>,
    pub confidence: Option<f64>,
    pub unit: Option<String>,
    pub level: Option<i64>,
    pub modality: Option<String>,
    pub scores: Option<Json>,
    pub provenance: Option<Json>,
    pub trust: Option<Json>,
    pub raw: Json,
}

/// The full result of `search`: hits plus telemetry and an optional page cursor.
#[derive(Clone, Debug)]
pub struct SearchResult {
    pub hits: Vec<Hit>,
    pub usage: Json,
    pub next_cursor: Option<String>,
}

fn hit_from_json(h: &Json) -> Hit {
    let id = match h.get("id") {
        Some(Json::Str(s)) => s.clone(),
        Some(other) => other.dump(),
        None => String::new(),
    };
    let opt = |k: &str| h.get(k).filter(|v| !v.is_null()).cloned();
    Hit {
        id,
        score: h.get("score").and_then(|v| v.as_f64()).unwrap_or(0.0),
        text: h.get("text").and_then(|v| v.as_str()).unwrap_or("").to_string(),
        citation: opt("citation"),
        confidence: h.get("confidence").and_then(|v| v.as_f64()),
        unit: h.get("unit").and_then(|v| v.as_str()).map(str::to_string),
        level: h.get("level").and_then(|v| v.as_i64()),
        modality: h.get("modality").and_then(|v| v.as_str()).map(str::to_string),
        scores: opt("scores"),
        provenance: opt("provenance"),
        trust: opt("trust"),
        raw: h.clone(),
    }
}

/// A connected RCP client.
pub struct Client {
    transport: Box<dyn Transport>,
    next_id: i64,
    protocol_version: i64,
    server: Json,
    caps: Json,
}

impl Client {
    // ── connection ──────────────────────────────────────────────────────────
    /// Spawn an RCP server subprocess and connect. `argv` e.g. `["python3", "srv.py"]`.
    pub fn connect_stdio<S: AsRef<str>>(argv: &[S]) -> Result<Client, RcpError> {
        let argv: Vec<String> = argv.iter().map(|s| s.as_ref().to_string()).collect();
        let t = StdioTransport::spawn(&argv)?;
        Client::from_transport(Box::new(t))
    }

    /// Connect to an HTTP RCP server, e.g. `"http://127.0.0.1:8000/rcp"`.
    pub fn connect_http(base_url: &str) -> Result<Client, RcpError> {
        Client::from_transport(Box::new(HttpTransport::new(base_url)))
    }

    fn from_transport(transport: Box<dyn Transport>) -> Result<Client, RcpError> {
        let mut c = Client {
            transport,
            next_id: 0,
            protocol_version: PROTOCOL_VERSION,
            server: Json::object(),
            caps: Json::object(),
        };
        let params = obj(&[
            ("protocolVersion", PROTOCOL_VERSION.into()),
            (
                "client",
                obj(&[("name", "rcp-rust".into()), ("version", "1.0".into())]),
            ),
            ("capabilities", Json::object()),
        ]);
        let res = c.request(Method::INITIALIZE, params)?;
        let pv = res
            .get("protocolVersion")
            .and_then(|v| v.as_i64())
            .unwrap_or(PROTOCOL_VERSION);
        if pv < MIN_PROTOCOL_VERSION {
            return Err(RcpError::new(
                Errc::VERSION_MISMATCH,
                "server offered no usable protocol version",
            ));
        }
        c.protocol_version = pv;
        c.server = res.get("server").cloned().unwrap_or_else(Json::object);
        c.caps = res.get("capabilities").cloned().unwrap_or_else(Json::object);
        Ok(c)
    }

    // ── introspection ───────────────────────────────────────────────────────
    pub fn protocol_version(&self) -> i64 {
        self.protocol_version
    }
    pub fn server(&self) -> &Json {
        &self.server
    }
    pub fn capabilities(&self) -> &Json {
        &self.caps
    }
    pub fn supports(&self, cap: Capability) -> bool {
        matches!(self.caps.get(cap.wire_key()), Some(v) if !v.is_null())
    }

    // ── typed, capability-gated calls ───────────────────────────────────────
    /// Dense embeddings → one `Vec<f64>` per input. `kind` selects asymmetric pooling.
    pub fn embed(&mut self, texts: &[&str], kind: Option<&str>) -> Result<Vec<Vec<f64>>, RcpError> {
        self.gate(Capability::Embed)?;
        let inputs: Vec<Json> = texts.iter().map(|t| Json::from(*t)).collect();
        let mut params = obj(&[("inputs", Json::Array(inputs))]);
        if let Some(k) = kind {
            params.insert("kind", k);
        }
        let r = self.request(Method::EMBED, params)?;
        let vectors = r.get("vectors").and_then(|v| v.as_array()).cloned().unwrap_or_default();
        Ok(vectors
            .iter()
            .map(|row| {
                row.as_array()
                    .map(|a| a.iter().filter_map(|x| x.as_f64()).collect())
                    .unwrap_or_default()
            })
            .collect())
    }

    /// Learned-sparse (SPLADE) terms → raw result `{ sparse: [...] }`.
    pub fn embed_sparse(&mut self, texts: &[&str]) -> Result<Json, RcpError> {
        self.gate(Capability::SparseEmbed)?;
        let inputs: Vec<Json> = texts.iter().map(|t| Json::from(*t)).collect();
        self.request(Method::EMBED_SPARSE, obj(&[("inputs", Json::Array(inputs))]))
    }

    /// Per-token multi-vector (ColBERT/ColPali) → `{ matrices, dimension }`.
    pub fn embed_multi(&mut self, inputs: Vec<Json>) -> Result<Json, RcpError> {
        self.gate(Capability::MultiVector)?;
        self.request(Method::EMBED_MULTI, obj(&[("inputs", Json::Array(inputs))]))
    }

    /// Cross-encoder rerank → one score per passage.
    pub fn rerank(&mut self, query: &str, passages: &[&str]) -> Result<Vec<f64>, RcpError> {
        self.gate(Capability::Rerank)?;
        let ps: Vec<Json> = passages.iter().map(|p| Json::from(*p)).collect();
        let params = obj(&[("query", query.into()), ("passages", Json::Array(ps))]);
        let r = self.request(Method::RERANK, params)?;
        let scores = r.get("scores").and_then(|v| v.as_array()).cloned().unwrap_or_default();
        Ok(scores.iter().filter_map(|x| x.as_f64()).collect())
    }

    /// Full retrieve → hits, usage, and an optional page cursor (spec §7.6).
    pub fn search(&mut self, query: &str, k: i64, opts: Option<Json>) -> Result<SearchResult, RcpError> {
        self.gate(Capability::Retrieve)?;
        if k < 1 {
            return Err(RcpError::new(Errc::INVALID_PARAMS, "value must be >= 1"));
        }
        let mut params = opts.unwrap_or_else(Json::object);
        params.insert("query", query);
        params.insert("k", k);
        let r = self.request(Method::RETRIEVE, params)?;
        let hits = r
            .get("hits")
            .and_then(|v| v.as_array())
            .map(|a| a.iter().map(hit_from_json).collect())
            .unwrap_or_default();
        Ok(SearchResult {
            hits,
            usage: r.get("usage").cloned().unwrap_or_else(Json::object),
            next_cursor: r.get("nextCursor").and_then(|v| v.as_str()).map(str::to_string),
        })
    }

    /// Convenience over `search` → just the hits.
    pub fn retrieve(&mut self, query: &str, k: i64) -> Result<Vec<Hit>, RcpError> {
        Ok(self.search(query, k, None)?.hits)
    }

    /// GraphRAG op ("local"|"global"|…) → raw result (spec §7.9).
    pub fn graph(&mut self, op: &str, params: Option<Json>) -> Result<Json, RcpError> {
        self.gate(Capability::Graph)?;
        let mut p = params.unwrap_or_else(Json::object);
        p.insert("op", op);
        self.request(Method::GRAPH, p)
    }

    /// Query transform (HyDE, expansion, …) → raw result (spec §7.8).
    pub fn transform(&mut self, query: &str, method: &str) -> Result<Json, RcpError> {
        self.gate(Capability::Transform)?;
        self.request(Method::TRANSFORM, obj(&[("query", query.into()), ("method", method.into())]))
    }

    /// Add documents to the corpus (spec §7.10).
    pub fn index_add(&mut self, params: Json) -> Result<Json, RcpError> {
        self.gate(Capability::Index)?;
        self.request(Method::INDEX_ADD, params)
    }

    /// Delete documents by id or filter (spec §7.11).
    pub fn index_delete(&mut self, params: Json) -> Result<Json, RcpError> {
        self.gate(Capability::Index)?;
        self.request(Method::INDEX_DELETE, params)
    }

    /// List downstream engines of an aggregator (spec §7.12).
    pub fn catalog(&mut self) -> Result<Json, RcpError> {
        self.gate(Capability::Catalog)?;
        self.request(Method::CATALOG_LIST, Json::object())
    }

    /// Send client→server relevance/reward/integrity signals (spec §7.16). Each
    /// signal REQUIRES `hitId`. Returns `{ accepted }`; side-effect only.
    pub fn feedback(&mut self, signals: Vec<Json>, opts: Option<Json>) -> Result<Json, RcpError> {
        self.gate(Capability::Feedback)?;
        for s in &signals {
            if s.get("hitId").filter(|v| !v.is_null()).is_none() {
                return Err(RcpError::new(
                    Errc::INVALID_PARAMS,
                    "each feedback signal requires 'hitId'",
                ));
            }
        }
        let mut p = opts.unwrap_or_else(Json::object);
        p.insert("signals", Json::Array(signals));
        self.request(Method::FEEDBACK, p)
    }

    /// Build or update a global/session memory (spec §7.17) → `{ memoryId, tokens? }`.
    pub fn memory_build(&mut self, params: Option<Json>) -> Result<Json, RcpError> {
        self.gate(Capability::Memory)?;
        self.request(Method::MEMORY_BUILD, params.unwrap_or_else(Json::object))
    }

    /// Recall clues / entry-points from a memory (spec §7.17) → `{ clues, hits? }`.
    pub fn memory_recall(&mut self, query: &str, opts: Option<Json>) -> Result<Json, RcpError> {
        self.gate(Capability::Memory)?;
        if let Some(n) = opts.as_ref().and_then(|o| o.get("n")).and_then(|v| v.as_i64()) {
            if n < 1 {
                return Err(RcpError::new(Errc::INVALID_PARAMS, "value must be >= 1"));
            }
        }
        let mut p = opts.unwrap_or_else(Json::object);
        p.insert("query", query);
        self.request(Method::MEMORY_RECALL, p)
    }

    /// Re-fetch identity + capabilities without re-initialising.
    pub fn info(&mut self) -> Result<Json, RcpError> {
        let r = self.request(Method::INFO, Json::object())?;
        self.server = r.get("server").cloned().unwrap_or_else(Json::object);
        self.caps = r.get("capabilities").cloned().unwrap_or_else(Json::object);
        Ok(r)
    }

    /// Escape hatch: invoke an arbitrary method with raw params.
    pub fn call(&mut self, method: &str, params: Json) -> Result<Json, RcpError> {
        self.request(method, params)
    }

    /// Liveness check; echoes `{ nonce }` when a nonce is given (spec §9).
    pub fn ping(&mut self, nonce: Option<Json>) -> Result<Json, RcpError> {
        let mut p = Json::object();
        if let Some(n) = nonce {
            p.insert("nonce", n);
        }
        self.request(Method::PING, p)
    }

    /// Politely ask the server to stop, then close the transport. Never errors.
    pub fn shutdown(&mut self) {
        let _ = self.request(Method::SHUTDOWN, Json::object());
        self.transport.close();
    }

    // ── internals ───────────────────────────────────────────────────────────
    fn gate(&self, cap: Capability) -> Result<(), RcpError> {
        if self.supports(cap) {
            Ok(())
        } else {
            Err(RcpError::new(
                Errc::CAPABILITY_MISSING,
                format!("server does not advertise '{}'", cap.wire_key()),
            ))
        }
    }

    fn request(&mut self, method: &str, params: Json) -> Result<Json, RcpError> {
        self.next_id += 1;
        let req = obj(&[
            ("jsonrpc", "2.0".into()),
            ("id", self.next_id.into()),
            ("method", method.into()),
            ("params", params),
        ]);
        let reply = self.transport.call(&req)?;
        if let Some(err) = reply.get("error") {
            if !err.is_null() {
                return Err(RcpError::from_json(err));
            }
        }
        Ok(reply.get("result").cloned().unwrap_or_else(Json::object))
    }
}
