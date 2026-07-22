// types.js — the RCP/1 wire vocabulary as plain JS values.
//
// Zero dependencies. Values mirror the type-theoretic C++ SDK and the native
// Python SDK byte-for-byte on the wire, so a Node client and a C++/Python server
// (or vice-versa) speak the exact same JSON-RPC.

// Protocol version negotiation (spec §7.1). We speak exactly version 1.
export const PROTOCOL_VERSION = 1;
export const MIN_PROTOCOL_VERSION = 1;

// min(peer, ours), clamped — mirrors rcp::negotiate_version.
export function negotiateVersion(theirs) {
  const t = Number.isInteger(theirs) ? theirs : PROTOCOL_VERSION;
  return t < PROTOCOL_VERSION ? t : PROTOCOL_VERSION;
}

// The capability lattice (spec §7.2). Each value IS the wire JSON key, so a
// capability is interchangeable with its string key — `supports(Capability.X)`
// and `supports("x")` are equivalent.
export const Capability = Object.freeze({
  Embed: "embed",
  SparseEmbed: "sparseEmbed",
  MultiVector: "multiVector",
  Rerank: "rerank",
  Retrieve: "retrieve",
  Transform: "transform",
  Graph: "graph",
  Index: "index",
  Catalog: "catalog",
});

// RCP/1 method names (spec §9).
export const Method = Object.freeze({
  INITIALIZE: "initialize",
  INFO: "info",
  EMBED: "embed",
  EMBED_SPARSE: "embed/sparse",
  EMBED_MULTI: "embed/multi",
  RERANK: "rerank",
  RETRIEVE: "retrieve",
  TRANSFORM: "query/transform",
  GRAPH: "graph",
  INDEX_ADD: "index/add",
  INDEX_DELETE: "index/delete",
  CATALOG_LIST: "catalog/list",
  CANCEL: "notifications/cancel",
  PROGRESS: "notifications/progress",
  LOG: "notifications/log",
  PING: "ping",
  SHUTDOWN: "shutdown",
});

// RCP/1 error codes (spec §12). -326xx are JSON-RPC; -320xx are RCP-specific.
export const Errc = Object.freeze({
  PARSE_ERROR: -32700,
  INVALID_REQUEST: -32600,
  METHOD_NOT_FOUND: -32601,
  INVALID_PARAMS: -32602,
  INTERNAL_ERROR: -32603,
  NOT_INITIALIZED: -32001,
  VERSION_MISMATCH: -32002,
  CAPABILITY_MISSING: -32003,
  UNKNOWN_METHOD: -32004,
  OPTION_UNSUPPORTED: -32005,
  CANCELLED: -32006,
  BACKEND_UNAVAILABLE: -32010,
  RATE_LIMITED: -32011,
});

// An RCP protocol or transport error, thrown by client calls. `message` is
// `"[RCP <code>] <detail>"` (matches the C++/Python SDKs); `detail` is the raw
// message and `data` the structured payload (spec §12).
export class RcpError extends Error {
  constructor(code, message, data = null) {
    super(`[RCP ${code}] ${message}`);
    this.name = "RcpError";
    this.code = code;
    this.detail = message;
    this.data = data;
  }

  // RateLimited / BackendUnavailable are transient by default; an explicit
  // `data.retryable` overrides the code-based default (spec §12).
  retryable() {
    if (this.data && typeof this.data.retryable === "boolean") return this.data.retryable;
    return this.code === Errc.RATE_LIMITED || this.code === Errc.BACKEND_UNAVAILABLE;
  }

  // Server-suggested backoff in ms (`data.retryAfterMs`), or -1.
  retryAfterMs() {
    if (this.data && Number.isInteger(this.data.retryAfterMs)) return this.data.retryAfterMs;
    return -1;
  }
}

// Which capability gates which method (spec §7.2 / §9). Used by both the client
// (pre-flight gating) and the server (dispatch gating).
export const CAP_FOR_METHOD = Object.freeze({
  [Method.EMBED]: Capability.Embed,
  [Method.EMBED_SPARSE]: Capability.SparseEmbed,
  [Method.EMBED_MULTI]: Capability.MultiVector,
  [Method.RERANK]: Capability.Rerank,
  [Method.RETRIEVE]: Capability.Retrieve,
  [Method.TRANSFORM]: Capability.Transform,
  [Method.GRAPH]: Capability.Graph,
  [Method.INDEX_ADD]: Capability.Index,
  [Method.INDEX_DELETE]: Capability.Index,
  [Method.CATALOG_LIST]: Capability.Catalog,
});
