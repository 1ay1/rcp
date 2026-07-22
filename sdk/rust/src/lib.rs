//! rcp — the Retrieval Context Protocol, a native Rust SDK.
//!
//! Zero dependencies (Rust standard library only). RCP is an open, versioned
//! JSON-RPC protocol so any RAG engine — any language, any vendor — can expose
//! embed / rerank / retrieve / graph / index / catalog, and any client can
//! consume it uniformly. This SDK speaks the exact same wire format as the C++,
//! Python, and Node.js SDKs, so a Rust client and a C++/Python/Node server (or
//! vice-versa) interoperate byte-for-byte.
//!
//! # Client
//!
//! ```no_run
//! use rcp::Capability;
//! let mut c = rcp::connect_stdio(&["python3", "my_server.py"])?;
//! if c.supports(Capability::Retrieve) {
//!     for hit in c.retrieve("eiffel tower", 3)? {
//!         println!("{} {}", hit.id, hit.score);
//!     }
//! }
//! # Ok::<(), rcp::RcpError>(())
//! ```
//!
//! # Server
//!
//! ```no_run
//! use rcp::{Server, Method, Capability, Json, obj};
//! let mut s = rcp::Server::new();
//! s.set_info("my-engine", "1.0");
//! s.advertise(Capability::Retrieve, obj(&[("maxK", 100.into())]));
//! s.on(Method::RETRIEVE, |params| {
//!     let q = params.get_str("query").unwrap_or("");
//!     Ok(obj(&[("hits", vec![obj(&[("id", "d1".into()), ("score", 0.9.into()), ("text", q.into())])].into())]))
//! });
//! s.serve_stdio();
//! ```

mod client;
mod json;
mod selector;
mod server;
mod transport;
mod types;

pub use client::{Client, Hit, SearchResult};
pub use json::{obj, Json};
pub use selector::{EngineSpec, Selector};
pub use server::{make_log_notification, make_progress_notification, Server};
pub use transport::{HttpTransport, StdioTransport, Transport};
pub use types::{
    cap_for_method, negotiate_version, Capability, Errc, Method, RcpError, MIN_PROTOCOL_VERSION,
    PROTOCOL_VERSION,
};

/// The SDK version.
pub const VERSION: &str = "1.0.0";

/// Spawn an RCP server subprocess and connect. `argv` e.g. `["python3", "srv.py"]`.
pub fn connect_stdio<S: AsRef<str>>(argv: &[S]) -> Result<Client, RcpError> {
    Client::connect_stdio(argv)
}

/// Connect to an HTTP RCP server, e.g. `"http://127.0.0.1:8000/rcp"`.
pub fn connect_http(base_url: &str) -> Result<Client, RcpError> {
    Client::connect_http(base_url)
}
