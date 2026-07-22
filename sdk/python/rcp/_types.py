"""Core RCP/1 wire vocabulary — protocol version, capabilities, methods, errors.

Pure Python, no dependencies. The values here mirror the type-theoretic C++ SDK
(``sdk/cpp/include/rcp/{types,protocol}.hpp``) byte-for-byte on the wire, so a
Python client and a C++ server (or vice-versa) speak the exact same JSON-RPC.
"""
from __future__ import annotations

import enum

# Protocol version negotiation (spec §7.1). We speak exactly version 1.
PROTOCOL_VERSION = 1
MIN_PROTOCOL_VERSION = 1


def negotiate_version(theirs) -> int:
    """min(peer, ours), clamped — mirrors rcp::negotiate_version."""
    try:
        t = int(theirs)
    except (TypeError, ValueError):
        t = PROTOCOL_VERSION
    return t if t < PROTOCOL_VERSION else PROTOCOL_VERSION


class Capability(enum.Enum):
    """The capability lattice (spec §7.2). ``.value`` is the wire JSON key, so a
    capability check is a total match on a typed enum, never string-poking."""
    Embed = "embed"
    SparseEmbed = "sparseEmbed"
    MultiVector = "multiVector"
    Rerank = "rerank"
    Retrieve = "retrieve"
    Transform = "transform"
    Graph = "graph"
    Index = "index"
    Session = "session"
    Feedback = "feedback"
    Memory = "memory"
    Catalog = "catalog"


class Method:
    """RCP/1 method names (spec §9). String constants — usable anywhere a wire
    method name is expected (``s.on(Method.RETRIEVE)`` or ``s.on("retrieve")``)."""
    INITIALIZE = "initialize"
    INFO = "info"
    EMBED = "embed"
    EMBED_SPARSE = "embed/sparse"
    EMBED_MULTI = "embed/multi"
    RERANK = "rerank"
    RETRIEVE = "retrieve"
    TRANSFORM = "query/transform"
    GRAPH = "graph"
    INDEX_ADD = "index/add"
    INDEX_DELETE = "index/delete"
    FEEDBACK = "feedback"
    MEMORY_BUILD = "memory/build"
    MEMORY_RECALL = "memory/recall"
    CATALOG_LIST = "catalog/list"
    CANCEL = "notifications/cancel"
    PROGRESS = "notifications/progress"
    LOG = "notifications/log"
    PING = "ping"
    SHUTDOWN = "shutdown"


class Errc:
    """RCP/1 error codes (spec §12). -326xx are JSON-RPC; -320xx are RCP-specific."""
    PARSE_ERROR = -32700
    INVALID_REQUEST = -32600
    METHOD_NOT_FOUND = -32601
    INVALID_PARAMS = -32602
    INTERNAL_ERROR = -32603
    NOT_INITIALIZED = -32001
    VERSION_MISMATCH = -32002
    CAPABILITY_MISSING = -32003
    UNKNOWN_METHOD = -32004
    OPTION_UNSUPPORTED = -32005
    CANCELLED = -32006
    BACKEND_UNAVAILABLE = -32010
    RATE_LIMITED = -32011


class RcpError(RuntimeError):
    """An RCP protocol or transport error, raised by client calls.

    Subclasses ``RuntimeError`` so existing ``except RuntimeError`` handlers keep
    working; ``str(e)`` is ``"[RCP <code>] <message>"`` (stable and greppable).
    Carries the structured ``data`` payload (spec §12) plus retry hints.
    """

    def __init__(self, code: int, message: str, data=None):
        super().__init__(f"[RCP {code}] {message}")
        self.code = code
        self.message = message
        self.data = data

    def retryable(self) -> bool:
        """RateLimited / BackendUnavailable are transient by default; an explicit
        ``data.retryable`` overrides the code-based default (spec §12)."""
        if isinstance(self.data, dict) and isinstance(self.data.get("retryable"), bool):
            return self.data["retryable"]
        return self.code in (Errc.RATE_LIMITED, Errc.BACKEND_UNAVAILABLE)

    def retry_after_ms(self) -> int:
        """Server-suggested backoff in ms (``data.retryAfterMs``), or -1."""
        if isinstance(self.data, dict) and isinstance(self.data.get("retryAfterMs"), int):
            return self.data["retryAfterMs"]
        return -1


# Which capability gates which method (spec §7.2 / §9). Used by both the client
# (pre-flight gating) and the server (dispatch gating).
CAP_FOR_METHOD = {
    Method.EMBED: Capability.Embed,
    Method.EMBED_SPARSE: Capability.SparseEmbed,
    Method.EMBED_MULTI: Capability.MultiVector,
    Method.RERANK: Capability.Rerank,
    Method.RETRIEVE: Capability.Retrieve,
    Method.TRANSFORM: Capability.Transform,
    Method.GRAPH: Capability.Graph,
    Method.INDEX_ADD: Capability.Index,
    Method.INDEX_DELETE: Capability.Index,
    Method.FEEDBACK: Capability.Feedback,
    Method.MEMORY_BUILD: Capability.Memory,
    Method.MEMORY_RECALL: Capability.Memory,
    Method.CATALOG_LIST: Capability.Catalog,
}


def cap_key(capability) -> str:
    """Wire JSON key for a Capability enum member (or a raw string passthrough)."""
    return capability.value if isinstance(capability, Capability) else str(capability)
