// client.js — connect to any RCP server (subprocess or HTTP), then typed calls.
//
// Mirrors the C++/Python `Client`: connecting performs the `initialize`
// handshake and caches the negotiated protocol version, server identity, and
// capabilities. Every typed call is capability-gated — a call to a feature the
// server never advertised throws before any round-trip.
//
// I/O is async (Node idiom): every call returns a Promise. Errors throw RcpError
// whose `.message` is `"[RCP <code>] <message>"`.

import { HttpTransport, StdioTransport } from "./transport.js";
import { Capability, Errc, MIN_PROTOCOL_VERSION, Method, PROTOCOL_VERSION, RcpError } from "./types.js";

const CLIENT_INFO = { name: "rcp-node", version: "1.0" };

// Normalise one retrieval Hit to `{ id, score, text[, citation] }` — the
// canonical hit shape (id coerced to string, extra fields dropped).
function hitToObject(h) {
  if (!h || typeof h !== "object") return { id: "", score: 0.0, text: String(h ?? "") };
  const rawId = h.id ?? "";
  const id = typeof rawId === "string" ? rawId : JSON.stringify(rawId);
  const out = { id, score: Number(h.score ?? 0.0), text: h.text ?? "" };
  if (h.citation !== undefined && h.citation !== null) out.citation = h.citation;
  return out;
}

export class Client {
  constructor() {
    this._transport = null;
    this._nextId = 0;
    this._protocolVersion = PROTOCOL_VERSION;
    this._server = { name: "unknown", version: "0" };
    this._caps = {};
  }

  // ── connection ────────────────────────────────────────────────────────────
  async connectStdio(argv) {
    this._transport = StdioTransport.spawn(Array.from(argv));
    await this._handshake();
    return this;
  }

  async connectHttp(baseUrl) {
    this._transport = new HttpTransport(baseUrl);
    await this._handshake();
    return this;
  }

  async _handshake() {
    const res = await this._request(Method.INITIALIZE, {
      protocolVersion: PROTOCOL_VERSION,
      client: { ...CLIENT_INFO },
      capabilities: {},
    });
    const pv = res.protocolVersion ?? PROTOCOL_VERSION;
    if (!Number.isInteger(pv) || pv < MIN_PROTOCOL_VERSION) {
      throw new RcpError(Errc.VERSION_MISMATCH, "server offered no usable protocol version");
    }
    this._protocolVersion = pv;
    const srv = res.server || {};
    this._server = { name: srv.name ?? "unknown", version: srv.version ?? "0" };
    this._caps = res.capabilities || {};
  }

  // ── introspection ─────────────────────────────────────────────────────────
  get protocolVersion() { return this._protocolVersion; }
  get server() { return this._server; }
  get capabilities() { return this._caps; }

  supports(capability) { return this._caps[capability] != null; }

  // ── typed, capability-gated calls ─────────────────────────────────────────

  // Dense embeddings -> number[][]. `kind` ("query"|"document") selects
  // asymmetric pooling.
  async embed(texts, kind = null) {
    this._gate(Capability.Embed);
    const p = { inputs: Array.from(texts) };
    if (kind) p.kind = kind;
    const r = await this._request(Method.EMBED, p);
    const vecs = r && typeof r === "object" && "vectors" in r ? r.vectors : r;
    return (vecs || []).map((row) => Array.from(row, Number));
  }

  // Learned-sparse (SPLADE) terms -> raw result `{ sparse: [...] }`.
  async embedSparse(texts) {
    this._gate(Capability.SparseEmbed);
    return this._request(Method.EMBED_SPARSE, { inputs: Array.from(texts) });
  }

  // Per-token multi-vector (ColBERT/ColPali) -> `{ matrices, dimension }`.
  async embedMulti(inputs) {
    this._gate(Capability.MultiVector);
    return this._request(Method.EMBED_MULTI, { inputs: Array.from(inputs) });
  }

  // Cross-encoder rerank -> number[] scores (one per passage).
  async rerank(query, passages) {
    this._gate(Capability.Rerank);
    const r = await this._request(Method.RERANK, { query, passages: Array.from(passages) });
    const scores = r && typeof r === "object" && "scores" in r ? r.scores : r;
    return (scores || []).map(Number);
  }

  // Full retrieve -> `{ hits, usage, nextCursor }` (spec §7.6).
  async search(query, k = 10, opts = null) {
    this._gate(Capability.Retrieve);
    if (k < 1) throw new RcpError(Errc.INVALID_PARAMS, "value must be >= 1");
    const p = opts ? { ...opts } : {};
    p.query = query;
    p.k = k;
    const r = await this._request(Method.RETRIEVE, p);
    const hits = r && typeof r === "object" ? r.hits || [] : [];
    return {
      hits: hits.map(hitToObject),
      usage: (r && r.usage) || {},
      nextCursor: (r && r.nextCursor) ?? null,
    };
  }

  // Convenience over `search` -> just the array of hit objects.
  async retrieve(query, k = 10, opts = null) {
    return (await this.search(query, k, opts)).hits;
  }

  // GraphRAG op ("local"|"global"|...) -> raw result (spec §7.9).
  async graph(op, params = null) {
    this._gate(Capability.Graph);
    const p = params ? { ...params } : {};
    p.op = op;
    return this._request(Method.GRAPH, p);
  }

  // Query transform (HyDE, expansion, ...) -> raw result (spec §7.8).
  async transform(query, method) {
    this._gate(Capability.Transform);
    return this._request(Method.TRANSFORM, { query, method });
  }

  // Add documents to the corpus (spec §7.10). `params` e.g. `{ documents: [...] }`.
  async indexAdd(params = null) {
    this._gate(Capability.Index);
    return this._request(Method.INDEX_ADD, params ? { ...params } : {});
  }

  // Delete documents by id (spec §7.11). `params` e.g. `{ ids: [...] }`.
  async indexDelete(params = null) {
    this._gate(Capability.Index);
    return this._request(Method.INDEX_DELETE, params ? { ...params } : {});
  }

  // List available sub-indexes / collections (spec §7.12).
  async catalog() {
    this._gate(Capability.Catalog);
    return this._request(Method.CATALOG_LIST, {});
  }

  // Re-fetch server identity + capabilities without re-initialising.
  async info() {
    const r = await this._request(Method.INFO, {});
    const srv = r.server || {};
    this._server = { name: srv.name ?? "unknown", version: srv.version ?? "0" };
    this._caps = r.capabilities || {};
    return r;
  }

  // Escape hatch: invoke an arbitrary method with raw params.
  async call(method, params = null) {
    return this._request(method, params ?? {});
  }

  // Liveness check; echoes `{ nonce }` if a nonce is given (spec §9).
  async ping(nonce = null) {
    const p = {};
    if (nonce !== null && nonce !== undefined) p.nonce = nonce;
    return this._request(Method.PING, p);
  }

  // Politely ask the server to stop, then close the transport. Never throws.
  async shutdown() {
    if (!this._transport) return;
    try {
      await this._request(Method.SHUTDOWN, {});
    } catch {
      // ignore — we are tearing down anyway
    } finally {
      this._transport.close();
      this._transport = null;
    }
  }

  // ── internals ─────────────────────────────────────────────────────────────
  _gate(capability) {
    if (!this.supports(capability)) {
      throw new RcpError(Errc.CAPABILITY_MISSING, `server does not advertise '${capability}'`);
    }
  }

  async _request(method, params) {
    if (!this._transport) throw new RcpError(Errc.BACKEND_UNAVAILABLE, "client is not connected");
    const id = ++this._nextId;
    const reply = await this._transport.call({ jsonrpc: "2.0", id, method, params });
    if (reply && typeof reply === "object") {
      if (reply.error != null) {
        throw new RcpError(reply.error.code ?? Errc.INTERNAL_ERROR, reply.error.message ?? "error", reply.error.data);
      }
      if ("result" in reply) return reply.result;
    }
    return {};
  }
}
