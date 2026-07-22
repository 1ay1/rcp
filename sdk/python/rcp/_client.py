"""RCP client — connect to any RCP server (subprocess or HTTP), then typed calls.

Mirrors the C++ ``rcp::Client`` (``sdk/cpp/include/rcp/client.hpp``): connecting
performs the ``initialize`` handshake and caches the negotiated protocol version,
server identity, and capabilities. Every typed call is **capability-gated** — a
call to a feature the server never advertised raises before any round-trip.

Errors raise :class:`RcpError` (a ``RuntimeError``) with ``str(e)`` of the form
``"[RCP <code>] <message>"``.
"""
from __future__ import annotations

import json

from ._transport import HttpTransport, StdioTransport
from ._types import (
    MIN_PROTOCOL_VERSION,
    PROTOCOL_VERSION,
    Capability,
    Errc,
    Method,
    RcpError,
    cap_key,
)

_CLIENT_INFO = {"name": "rcp-python", "version": "1.0"}


def _hit_to_dict(h) -> dict:
    """Normalise one retrieval Hit to ``{id, score, text[, citation]}`` — the
    canonical hit shape (id coerced to str, extra fields dropped)."""
    if not isinstance(h, dict):
        return {"id": "", "score": 0.0, "text": str(h)}
    raw_id = h.get("id", "")
    hit_id = raw_id if isinstance(raw_id, str) else json.dumps(raw_id, separators=(",", ":"))
    out = {"id": hit_id, "score": float(h.get("score", 0.0)), "text": h.get("text", "")}
    if h.get("citation") is not None:
        out["citation"] = h["citation"]
    return out


class Client:
    """A connected RCP client. Obtain one via :func:`rcp.connect_stdio` /
    :func:`rcp.connect_http`, or construct and call ``connect_stdio`` /
    ``connect_http`` directly — either style works."""

    def __init__(self):
        self._transport = None
        self._next_id = 0
        self._protocol_version = PROTOCOL_VERSION
        self._server = {"name": "unknown", "version": "0"}
        self._caps: dict = {}

    # ── connection ──────────────────────────────────────────────────────────
    def connect_stdio(self, argv) -> "Client":
        self._transport = StdioTransport.spawn(list(argv))
        self._handshake()
        return self

    def connect_http(self, base_url: str) -> "Client":
        self._transport = HttpTransport(base_url)
        self._handshake()
        return self

    def _handshake(self) -> None:
        res = self._request(Method.INITIALIZE, {
            "protocolVersion": PROTOCOL_VERSION,
            "client": dict(_CLIENT_INFO),
            "capabilities": {},
        })
        pv = res.get("protocolVersion", PROTOCOL_VERSION)
        if not isinstance(pv, int) or pv < MIN_PROTOCOL_VERSION:
            raise RcpError(Errc.VERSION_MISMATCH, "server offered no usable protocol version")
        self._protocol_version = pv
        srv = res.get("server", {}) or {}
        self._server = {"name": srv.get("name", "unknown"), "version": srv.get("version", "0")}
        self._caps = res.get("capabilities", {}) or {}

    # ── introspection ───────────────────────────────────────────────────────
    @property
    def protocol_version(self) -> int:
        return self._protocol_version

    @property
    def server(self) -> dict:
        return self._server

    @property
    def capabilities(self) -> dict:
        return self._caps

    def supports(self, capability) -> bool:
        return self._caps.get(cap_key(capability)) is not None

    # ── typed, capability-gated calls ───────────────────────────────────────
    def embed(self, texts, kind=None):
        """Dense embeddings → ``list[list[float]]``. ``kind`` ("query"|"document")
        selects asymmetric pooling."""
        self._gate(Capability.Embed)
        p = {"inputs": list(texts)}
        if kind:
            p["kind"] = kind
        r = self._request(Method.EMBED, p)
        vecs = r.get("vectors", r) if isinstance(r, dict) else r
        return [[float(x) for x in row] for row in vecs]

    def embed_sparse(self, texts):
        """Learned-sparse (SPLADE) terms → raw result ``{sparse: [...]}``."""
        self._gate(Capability.SparseEmbed)
        return self._request(Method.EMBED_SPARSE, {"inputs": list(texts)})

    def embed_multi(self, inputs):
        """Per-token multi-vector (ColBERT/ColPali) → ``{matrices, dimension}``."""
        self._gate(Capability.MultiVector)
        return self._request(Method.EMBED_MULTI, {"inputs": list(inputs)})

    def rerank(self, query, passages):
        """Cross-encoder rerank → ``list[float]`` scores (one per passage)."""
        self._gate(Capability.Rerank)
        r = self._request(Method.RERANK, {"query": query, "passages": list(passages)})
        scores = r.get("scores", r) if isinstance(r, dict) else r
        return [float(x) for x in scores]

    def search(self, query, k=10, opts=None):
        """Full retrieve → ``{hits, usage, next_cursor}`` (spec §7.6)."""
        self._gate(Capability.Retrieve)
        if k < 1:
            raise RcpError(Errc.INVALID_PARAMS, "value must be >= 1")
        p = dict(opts) if opts else {}
        p["query"] = query
        p["k"] = k
        r = self._request(Method.RETRIEVE, p)
        hits = r.get("hits", []) if isinstance(r, dict) else []
        return {
            "hits": [_hit_to_dict(h) for h in hits],
            "usage": (r.get("usage", {}) if isinstance(r, dict) else {}),
            "next_cursor": (r.get("nextCursor") if isinstance(r, dict) else None),
        }

    def retrieve(self, query, k=10, opts=None):
        """Convenience over :meth:`search` → just the ``list[dict]`` of hits."""
        return self.search(query, k, opts)["hits"]

    def graph(self, op, params=None):
        """GraphRAG op ("local"|"global"|...) → raw result dict (spec §7.9)."""
        self._gate(Capability.Graph)
        p = dict(params) if params else {}
        p["op"] = op
        return self._request(Method.GRAPH, p)

    def transform(self, query, method):
        """Query transform (HyDE, expansion, ...) → raw result (spec §7.8)."""
        self._gate(Capability.Transform)
        return self._request(Method.TRANSFORM, {"query": query, "method": method})

    def index_add(self, params=None):
        """Add documents to the corpus (spec §7.10). ``params`` e.g. ``{documents:[...]}``."""
        self._gate(Capability.Index)
        return self._request(Method.INDEX_ADD, dict(params) if params else {})

    def index_delete(self, params=None):
        """Delete documents by id (spec §7.11). ``params`` e.g. ``{ids:[...]}``."""
        self._gate(Capability.Index)
        return self._request(Method.INDEX_DELETE, dict(params) if params else {})

    def catalog(self):
        """List available sub-indexes / collections (spec §7.12)."""
        self._gate(Capability.Catalog)
        return self._request(Method.CATALOG_LIST, {})

    def info(self):
        """Re-fetch server identity + capabilities without re-initialising."""
        r = self._request(Method.INFO, {})
        srv = r.get("server", {}) or {}
        self._server = {"name": srv.get("name", "unknown"), "version": srv.get("version", "0")}
        self._caps = r.get("capabilities", {}) or {}
        return r

    def call(self, method, params=None):
        """Escape hatch: invoke an arbitrary method with raw params."""
        return self._request(method, params if params is not None else {})

    def ping(self, nonce=None):
        """Liveness check; echoes ``{nonce}`` if a nonce is given (spec §9)."""
        p = {}
        if nonce is not None:
            p["nonce"] = nonce
        return self._request(Method.PING, p)

    def shutdown(self) -> None:
        """Politely ask the server to stop, then close the transport. Never raises."""
        if self._transport is None:
            return
        try:
            self._request(Method.SHUTDOWN, {})
        except RcpError:
            pass
        finally:
            self._transport.close()
            self._transport = None

    # ── internals ───────────────────────────────────────────────────────────
    def _gate(self, capability) -> None:
        if not self.supports(capability):
            raise RcpError(Errc.CAPABILITY_MISSING,
                           f"server does not advertise '{cap_key(capability)}'")

    def _request(self, method, params):
        if self._transport is None:
            raise RcpError(Errc.BACKEND_UNAVAILABLE, "client is not connected")
        self._next_id += 1
        reply = self._transport.call({
            "jsonrpc": "2.0", "id": self._next_id, "method": method, "params": params,
        })
        if isinstance(reply, dict):
            err = reply.get("error")
            if err is not None:
                raise RcpError(err.get("code", Errc.INTERNAL_ERROR),
                               err.get("message", "error"), err.get("data"))
            if "result" in reply:
                return reply["result"]
        return {}
