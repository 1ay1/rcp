//! types.rs — the RCP/1 wire vocabulary: protocol version, capabilities,
//! methods, error codes, and the typed error. Values mirror the C++, Python, and
//! Node.js SDKs byte-for-byte on the wire.

use crate::json::Json;
use std::fmt;

/// The protocol version this SDK speaks.
pub const PROTOCOL_VERSION: i64 = 1;
/// The lowest protocol version this SDK will accept.
pub const MIN_PROTOCOL_VERSION: i64 = 1;

/// `min(peer, ours)`, clamped — mirrors `rcp::negotiate_version`.
pub fn negotiate_version(theirs: i64) -> i64 {
    if theirs < PROTOCOL_VERSION {
        theirs
    } else {
        PROTOCOL_VERSION
    }
}

/// The capability lattice (spec §7.2). Each variant maps to a wire JSON key.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Capability {
    Embed,
    SparseEmbed,
    MultiVector,
    Rerank,
    Retrieve,
    Transform,
    Graph,
    Index,
    Session,
    Feedback,
    Memory,
    Catalog,
}

impl Capability {
    /// The wire JSON key for this capability.
    pub fn wire_key(self) -> &'static str {
        match self {
            Capability::Embed => "embed",
            Capability::SparseEmbed => "sparseEmbed",
            Capability::MultiVector => "multiVector",
            Capability::Rerank => "rerank",
            Capability::Retrieve => "retrieve",
            Capability::Transform => "transform",
            Capability::Graph => "graph",
            Capability::Index => "index",
            Capability::Session => "session",
            Capability::Feedback => "feedback",
            Capability::Memory => "memory",
            Capability::Catalog => "catalog",
        }
    }

    /// Every capability, for iteration.
    pub const ALL: [Capability; 12] = [
        Capability::Embed,
        Capability::SparseEmbed,
        Capability::MultiVector,
        Capability::Rerank,
        Capability::Retrieve,
        Capability::Transform,
        Capability::Graph,
        Capability::Index,
        Capability::Session,
        Capability::Feedback,
        Capability::Memory,
        Capability::Catalog,
    ];
}

/// RCP/1 method names (spec §9). String constants — usable anywhere a method
/// name is expected (`server.on(Method::RETRIEVE, …)` or `server.on("retrieve", …)`).
pub struct Method;

impl Method {
    pub const INITIALIZE: &'static str = "initialize";
    pub const INFO: &'static str = "info";
    pub const EMBED: &'static str = "embed";
    pub const EMBED_SPARSE: &'static str = "embed/sparse";
    pub const EMBED_MULTI: &'static str = "embed/multi";
    pub const RERANK: &'static str = "rerank";
    pub const RETRIEVE: &'static str = "retrieve";
    pub const TRANSFORM: &'static str = "query/transform";
    pub const GRAPH: &'static str = "graph";
    pub const INDEX_ADD: &'static str = "index/add";
    pub const INDEX_DELETE: &'static str = "index/delete";
    pub const FEEDBACK: &'static str = "feedback";
    pub const MEMORY_BUILD: &'static str = "memory/build";
    pub const MEMORY_RECALL: &'static str = "memory/recall";
    pub const CATALOG_LIST: &'static str = "catalog/list";
    pub const CANCEL: &'static str = "notifications/cancel";
    pub const PROGRESS: &'static str = "notifications/progress";
    pub const LOG: &'static str = "notifications/log";
    pub const PING: &'static str = "ping";
    pub const SHUTDOWN: &'static str = "shutdown";
}

/// RCP/1 error codes (spec §12). `-326xx` are JSON-RPC; `-320xx` are RCP-specific.
pub struct Errc;

impl Errc {
    pub const PARSE_ERROR: i64 = -32700;
    pub const INVALID_REQUEST: i64 = -32600;
    pub const METHOD_NOT_FOUND: i64 = -32601;
    pub const INVALID_PARAMS: i64 = -32602;
    pub const INTERNAL_ERROR: i64 = -32603;
    pub const NOT_INITIALIZED: i64 = -32001;
    pub const VERSION_MISMATCH: i64 = -32002;
    pub const CAPABILITY_MISSING: i64 = -32003;
    pub const UNKNOWN_METHOD: i64 = -32004;
    pub const OPTION_UNSUPPORTED: i64 = -32005;
    pub const CANCELLED: i64 = -32006;
    pub const BACKEND_UNAVAILABLE: i64 = -32010;
    pub const RATE_LIMITED: i64 = -32011;
}

/// An RCP protocol or transport error.
///
/// `Display` renders `"[RCP <code>] <message>"` (matching every other SDK), and
/// the error carries the structured `data` payload (spec §12) plus retry hints.
#[derive(Clone, Debug)]
pub struct RcpError {
    pub code: i64,
    pub message: String,
    pub data: Option<Json>,
}

impl RcpError {
    pub fn new(code: i64, message: impl Into<String>) -> RcpError {
        RcpError {
            code,
            message: message.into(),
            data: None,
        }
    }

    pub fn with_data(code: i64, message: impl Into<String>, data: Json) -> RcpError {
        RcpError {
            code,
            message: message.into(),
            data: Some(data),
        }
    }

    /// RateLimited / BackendUnavailable are transient by default; an explicit
    /// `data.retryable` overrides the code-based default (spec §12).
    pub fn retryable(&self) -> bool {
        if let Some(d) = &self.data {
            if let Some(b) = d.get("retryable").and_then(|v| v.as_bool()) {
                return b;
            }
        }
        self.code == Errc::RATE_LIMITED || self.code == Errc::BACKEND_UNAVAILABLE
    }

    /// Server-suggested backoff in ms (`data.retryAfterMs`), or `-1`.
    pub fn retry_after_ms(&self) -> i64 {
        self.data
            .as_ref()
            .and_then(|d| d.get("retryAfterMs"))
            .and_then(|v| v.as_i64())
            .unwrap_or(-1)
    }

    /// Build an `RcpError` from a JSON-RPC `error` object.
    pub fn from_json(err: &Json) -> RcpError {
        RcpError {
            code: err.get("code").and_then(|v| v.as_i64()).unwrap_or(Errc::INTERNAL_ERROR),
            message: err
                .get("message")
                .and_then(|v| v.as_str())
                .unwrap_or("error")
                .to_string(),
            data: err.get("data").cloned(),
        }
    }
}

impl fmt::Display for RcpError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "[RCP {}] {}", self.code, self.message)
    }
}

impl std::error::Error for RcpError {}

/// Which capability gates which method (spec §7.2 / §9).
pub fn cap_for_method(method: &str) -> Option<Capability> {
    Some(match method {
        Method::EMBED => Capability::Embed,
        Method::EMBED_SPARSE => Capability::SparseEmbed,
        Method::EMBED_MULTI => Capability::MultiVector,
        Method::RERANK => Capability::Rerank,
        Method::RETRIEVE => Capability::Retrieve,
        Method::TRANSFORM => Capability::Transform,
        Method::GRAPH => Capability::Graph,
        Method::INDEX_ADD | Method::INDEX_DELETE => Capability::Index,
        Method::FEEDBACK => Capability::Feedback,
        Method::MEMORY_BUILD | Method::MEMORY_RECALL => Capability::Memory,
        Method::CATALOG_LIST => Capability::Catalog,
        _ => return None,
    })
}
