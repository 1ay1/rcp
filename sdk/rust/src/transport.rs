//! transport.rs — the request/reply seam (subprocess pipe or HTTP/1.1).
//!
//! A `Transport` is a total function `&Json -> Result<Json, RcpError>`. Two
//! concrete transports mirror the other SDKs, both built on `std` alone:
//!
//!   * `StdioTransport` — newline-delimited JSON over a child's stdin/stdout.
//!   * `HttpTransport`   — minimal HTTP/1.1 POST to `<base>/<method>`.

use crate::json::Json;
use crate::types::{Errc, RcpError};
use std::io::{BufRead, BufReader, Read, Write};
use std::net::TcpStream;
use std::process::{Child, ChildStdin, ChildStdout, Command, Stdio};

/// One JSON-RPC request → one reply.
pub trait Transport {
    fn call(&mut self, request: &Json) -> Result<Json, RcpError>;
    fn close(&mut self);
}

/// Newline-delimited JSON over a subprocess' stdio. Replies are correlated by
/// JSON-RPC id; server notifications (frames carrying `method`) and stray
/// mismatched-id replies are skipped (spec §4.7).
pub struct StdioTransport {
    child: Child,
    stdin: ChildStdin,
    stdout: BufReader<ChildStdout>,
}

impl StdioTransport {
    pub fn spawn(argv: &[String]) -> Result<StdioTransport, RcpError> {
        if argv.is_empty() {
            return Err(RcpError::new(
                Errc::INVALID_PARAMS,
                "server command must be non-empty",
            ));
        }
        let mut child = Command::new(&argv[0])
            .args(&argv[1..])
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            // stderr inherits: server diagnostics reach the launching terminal.
            .spawn()
            .map_err(|e| {
                RcpError::new(
                    Errc::BACKEND_UNAVAILABLE,
                    format!("cannot spawn {}: {}", argv[0], e),
                )
            })?;
        let stdin = child.stdin.take().expect("piped stdin");
        let stdout = BufReader::new(child.stdout.take().expect("piped stdout"));
        Ok(StdioTransport {
            child,
            stdin,
            stdout,
        })
    }
}

impl Transport for StdioTransport {
    fn call(&mut self, request: &Json) -> Result<Json, RcpError> {
        let mut line = request.dump();
        line.push('\n');
        self.stdin
            .write_all(line.as_bytes())
            .and_then(|_| self.stdin.flush())
            .map_err(|_| RcpError::new(Errc::BACKEND_UNAVAILABLE, "write to server failed"))?;

        let want_id = request.get("id").cloned();
        loop {
            let mut buf = String::new();
            let n = self.stdout.read_line(&mut buf).map_err(|e| {
                RcpError::new(Errc::BACKEND_UNAVAILABLE, format!("read failed: {}", e))
            })?;
            if n == 0 {
                return Err(RcpError::new(
                    Errc::BACKEND_UNAVAILABLE,
                    "server closed the connection",
                ));
            }
            let trimmed = buf.trim();
            if trimmed.is_empty() {
                continue;
            }
            let reply = Json::parse(trimmed).map_err(|e| RcpError::new(Errc::PARSE_ERROR, e))?;
            if reply.contains_key("method") {
                continue; // server-initiated notification
            }
            if let (Some(want), Some(got)) = (&want_id, reply.get("id")) {
                if want != got {
                    continue; // stray reply for an earlier request
                }
            }
            return Ok(reply);
        }
    }

    fn close(&mut self) {
        let _ = self.stdin.flush();
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

impl Drop for StdioTransport {
    fn drop(&mut self) {
        // Never orphan the child: std's Child::drop detaches by default.
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

/// Minimal HTTP/1.1 client: POST the request to `<base_url>/<method>`. The server
/// ignores the path and dispatches on the body's `method`. Maps HTTP 429 →
/// RateLimited and 503 → BackendUnavailable (spec §5.2).
pub struct HttpTransport {
    base: String,
}

impl HttpTransport {
    pub fn new(base_url: &str) -> HttpTransport {
        HttpTransport {
            base: base_url.trim_end_matches('/').to_string(),
        }
    }
}

impl Transport for HttpTransport {
    fn call(&mut self, request: &Json) -> Result<Json, RcpError> {
        let method = request.get_str("method").unwrap_or("");
        let url = format!("{}/{}", self.base, method);
        let (host, port, path) = parse_http_url(&url)?;
        let body = request.dump();
        let req = format!(
            "POST {} HTTP/1.1\r\nHost: {}\r\nContent-Type: application/json\r\n\
             Content-Length: {}\r\nConnection: close\r\n\r\n{}",
            path,
            host,
            body.len(),
            body
        );
        let mut stream = TcpStream::connect((host.as_str(), port))
            .map_err(|e| RcpError::new(Errc::BACKEND_UNAVAILABLE, format!("connect failed: {}", e)))?;
        stream
            .write_all(req.as_bytes())
            .map_err(|e| RcpError::new(Errc::BACKEND_UNAVAILABLE, format!("write failed: {}", e)))?;
        let mut resp = Vec::new();
        stream
            .read_to_end(&mut resp)
            .map_err(|e| RcpError::new(Errc::BACKEND_UNAVAILABLE, format!("read failed: {}", e)))?;

        let (status, body_bytes) = parse_http_response(&resp)?;
        if status == 429 {
            return Err(RcpError::new(Errc::RATE_LIMITED, "HTTP 429"));
        }
        if status == 503 {
            return Err(RcpError::new(Errc::BACKEND_UNAVAILABLE, "HTTP 503"));
        }
        let text = String::from_utf8_lossy(body_bytes);
        if !(200..300).contains(&status) && text.trim().is_empty() {
            return Err(RcpError::new(
                Errc::BACKEND_UNAVAILABLE,
                format!("HTTP {}", status),
            ));
        }
        Json::parse(text.trim()).map_err(|e| RcpError::new(Errc::PARSE_ERROR, e))
    }

    fn close(&mut self) {}
}

/// Split `http://host[:port]/path` into `(host, port, path)`.
fn parse_http_url(url: &str) -> Result<(String, u16, String), RcpError> {
    let rest = url
        .strip_prefix("http://")
        .ok_or_else(|| RcpError::new(Errc::INVALID_PARAMS, "only http:// URLs are supported"))?;
    let (authority, path) = match rest.find('/') {
        Some(i) => (&rest[..i], &rest[i..]),
        None => (rest, "/"),
    };
    let (host, port) = match authority.rsplit_once(':') {
        Some((h, p)) => (
            h.to_string(),
            p.parse::<u16>()
                .map_err(|_| RcpError::new(Errc::INVALID_PARAMS, "bad port"))?,
        ),
        None => (authority.to_string(), 80),
    };
    Ok((host, port, path.to_string()))
}

/// Return `(status_code, body_slice)` from a raw HTTP/1.1 response.
fn parse_http_response(resp: &[u8]) -> Result<(u16, &[u8]), RcpError> {
    let sep = find_subslice(resp, b"\r\n\r\n")
        .ok_or_else(|| RcpError::new(Errc::BACKEND_UNAVAILABLE, "malformed HTTP response"))?;
    let head = &resp[..sep];
    let body = &resp[sep + 4..];
    // Status line: "HTTP/1.1 200 OK"
    let line_end = find_subslice(head, b"\r\n").unwrap_or(head.len());
    let status_line = String::from_utf8_lossy(&head[..line_end]);
    let status = status_line
        .split_whitespace()
        .nth(1)
        .and_then(|s| s.parse::<u16>().ok())
        .ok_or_else(|| RcpError::new(Errc::BACKEND_UNAVAILABLE, "no HTTP status"))?;
    Ok((status, body))
}

fn find_subslice(haystack: &[u8], needle: &[u8]) -> Option<usize> {
    haystack
        .windows(needle.len())
        .position(|w| w == needle)
}
