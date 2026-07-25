#!/usr/bin/env python3
"""Example RCP/1 server — a complete, dependency-free HYBRID retrieval engine.

Run as a subprocess (stdio, the default):
    python3 examples/example_server.py
Or over HTTP:
    python3 examples/example_server.py --http 8000

This is a faithful miniature of a production RAG pipeline (spec §3), built with
nothing but the standard library so you can read the whole thing:

    query ─▶ dense recall  ┐
            sparse recall  ├─▶ RRF fusion ─▶ rerank ─▶ top-k hits + citations
                           ┘   (§16.3)      (§7.6)      (the funnel, §3.3)

It advertises **embed + sparseEmbed + rerank + retrieve + graph**, supports
`mode: dense|sparse|hybrid`, honours the `candidateK ≥ rerank.topN ≥ k` funnel,
returns per-stage `scores` and `usage`, and certifies at conformance **L2**.
Swap the toy embedders for sentence-transformers / SPLADE / FAISS / a
cross-encoder and a real cross-encoder — the wire contract never changes.
"""
import hashlib
import math
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "sdk", "python"))

import rcp  # noqa: E402
from rcp import rrf_fuse  # the SDK's reference fusion (spec §16.3)  # noqa: E402

DIM = 384
DOCS = [
    ("d1", "The Eiffel Tower is a wrought-iron lattice tower on the Champ de Mars in Paris, France.", "https://en.wikipedia.org/wiki/Eiffel_Tower"),
    ("d2", "Photosynthesis converts light energy into chemical energy stored in glucose within plant chloroplasts.", "https://en.wikipedia.org/wiki/Photosynthesis"),
    ("d3", "The Great Wall of China stretches thousands of kilometres across the country's northern borders.", "https://en.wikipedia.org/wiki/Great_Wall_of_China"),
    ("d4", "Paris is the capital of France and sits on the river Seine in the north of the country.", "https://en.wikipedia.org/wiki/Paris"),
    ("d5", "Iron is a chemical element; wrought iron is a tough, malleable form used in historic construction.", "https://en.wikipedia.org/wiki/Wrought_iron"),
]

_WORD = re.compile(r"[a-z0-9]+")


def _tokens(text):
    return _WORD.findall(text.lower())


# ── dense embedder: hashed bag-of-words, L2-normalised (stands in for a model) ──
def embed_dense(text):
    v = [0.0] * DIM
    for tok in _tokens(text):
        h = int(hashlib.blake2b(tok.encode(), digest_size=8).hexdigest(), 16)
        v[h % DIM] += 1.0
    n = math.sqrt(sum(x * x for x in v)) or 1.0
    return [x / n for x in v]


# ── sparse embedder: term → weight, SPLADE-style (indices are hashed vocab ids) ─
def embed_sparse(text):
    weights = {}
    for tok in _tokens(text):
        idx = int(hashlib.blake2b(tok.encode(), digest_size=4).hexdigest(), 16)
        weights[idx] = weights.get(idx, 0.0) + 1.0
    items = sorted(weights.items())
    return {"indices": [i for i, _ in items], "values": [w for _, w in items]}


_DENSE = {uri: embed_dense(text) for uri, text, _ in DOCS}
_SPARSE = {uri: embed_sparse(text) for uri, text, _ in DOCS}
_TEXT = {uri: text for uri, text, _ in DOCS}
_URI = {uri: src for uri, _, src in DOCS}


def _cosine(a, b):
    return sum(x * y for x, y in zip(a, b))


def _sparse_dot(q, d):
    dv = dict(zip(d["indices"], d["values"]))
    return sum(w * dv.get(i, 0.0) for i, w in zip(q["indices"], q["values"]))


def _dense_ranked(query, n):
    q = embed_dense(query)
    scored = sorted(((_cosine(q, _DENSE[u]), u) for u in _DENSE), reverse=True)
    return [{"id": u, "score": s} for s, u in scored[:n]]


def _sparse_ranked(query, n):
    q = embed_sparse(query)
    scored = sorted(((_sparse_dot(q, _SPARSE[u]), u) for u in _SPARSE), reverse=True)
    return [{"id": u, "score": s} for s, u in scored[:n] if s > 0] or \
           [{"id": u, "score": s} for s, u in scored[:n]]


def _rerank_score(query, text):
    # A toy cross-encoder: lexical overlap × length prior. Stands in for a real
    # query-document interaction model (§7.6). Deterministic and in [0,1].
    q, d = set(_tokens(query)), set(_tokens(text))
    if not q:
        return 0.0
    return len(q & d) / len(q)


def _retrieve(query, k, candidate_k, top_n, mode):
    """Run the funnel: recall (mode) → fuse → rerank → top-k (spec §3, §7.7)."""
    # 1. Recall — wide and cheap. `candidateK` candidates per enabled retriever.
    lists = {}
    if mode in ("dense", "hybrid"):
        lists["dense"] = _dense_ranked(query, candidate_k)
    if mode in ("sparse", "hybrid"):
        lists["sparse"] = _sparse_ranked(query, candidate_k)

    # 2. Fuse — rank-based, so dense cosine and sparse dot never get compared
    #    directly. Single-retriever modes pass through fusion unchanged.
    fused = rrf_fuse(lists, k=None)
    per_stage = {u["id"]: dict(u.get("meta", {})) for u in fused}

    # 3. Rerank — precise and narrow. Rescore the top `top_n` candidates.
    head = fused[:top_n]
    for h in head:
        h["_rr"] = _rerank_score(query, _TEXT[h["id"]])
    head.sort(key=lambda h: h["_rr"], reverse=True)

    # 4. Pack + cite — return the best `k`, richest body first.
    hits = []
    for h in head[:k]:
        uri = h["id"]
        stage = per_stage.get(uri, {})
        hits.append({
            "id": uri,
            "score": round(h["_rr"], 4),
            "text": _TEXT[uri],
            "scores": {kk: round(vv, 4) for kk, vv in stage.items()
                       if isinstance(vv, (int, float))} | {"rerank": round(h["_rr"], 4)},
            "citation": {"source": uri, "uri": _URI[uri], "title": _TEXT[uri][:40] + "…"},
        })
    usage = {"candidates": len(fused), "reranked": len(head), "mode": mode,
             "retrievers": list(lists.keys())}
    return {"hits": hits, "usage": usage, "indexVersion": "demo-1"}


def build():
    s = rcp.Server()
    s.set_info("rcp-example", "1.0.0")
    # Declare L2 honestly — the conformance harness will hold us to it.
    s.set_conformance("L2")
    s.advertise(rcp.Capability.Embed, {"dimension": DIM, "modalities": ["text"],
                                       "encodings": ["json", "f32-base64"]})
    s.advertise(rcp.Capability.SparseEmbed, {"identity": "demo-splade"})
    s.advertise(rcp.Capability.Rerank, {"methods": ["cross-encoder"], "maxCandidates": 100})
    s.advertise(rcp.Capability.Retrieve, {"maxK": 100, "modes": ["dense", "sparse", "hybrid"],
                                          "citations": True, "defaultCandidateK": 50})
    s.advertise(rcp.Capability.Graph, {"ops": ["local", "global"]})

    @s.on(rcp.Method.EMBED)
    def _embed(params):
        items = params.get("inputs") or params.get("texts", [])
        texts = [t if isinstance(t, str) else t.get("text", "") for t in items]
        vecs = [embed_dense(t) for t in texts]
        encoding = params.get("encoding", "json")
        payload, meta = rcp.encode_vectors(vecs, encoding)
        return {"vectors": payload, **meta}

    @s.on(rcp.Method.EMBED_SPARSE)
    def _embed_sparse(params):
        items = params.get("texts") or params.get("inputs", [])
        return {"sparse": [embed_sparse(t if isinstance(t, str) else t.get("text", ""))
                           for t in items]}

    @s.on(rcp.Method.RERANK)
    def _rerank(params):
        query = params.get("query", "")
        docs = params.get("documents", [])
        top_n = int(params.get("topN", len(docs)))
        scored = sorted(
            ({"index": i, "score": round(_rerank_score(query, d if isinstance(d, str) else d.get("text", "")), 4)}
             for i, d in enumerate(docs)),
            key=lambda r: r["score"], reverse=True)
        return {"results": scored[:top_n]}

    @s.on(rcp.Method.RETRIEVE)
    def _retrieve_handler(params):
        k = int(params.get("k", 5))
        mode = params.get("mode", "hybrid")
        # Funnel defaults: candidateK ≥ topN ≥ k, all server-chosen when absent.
        candidate_k = int(params.get("candidateK", max(k, min(50, len(DOCS)))))
        rr = params.get("rerank") or {}
        top_n = int(rr.get("topN", max(k, min(candidate_k, 20))))
        if mode not in ("dense", "sparse", "hybrid"):
            raise rcp.RcpError(rcp.Errc.OPTION_UNSUPPORTED, f"mode '{mode}' not supported",
                               {"option": "mode"})
        return _retrieve(params.get("query", ""), k, candidate_k, top_n, mode)

    @s.on(rcp.Method.GRAPH)
    def _graph(params):
        op = params.get("op", "local")
        if op == "local":
            r = _retrieve(params.get("query", ""), int(params.get("k", 5)),
                          candidate_k=len(DOCS), top_n=len(DOCS), mode="hybrid")
            return {"hits": r["hits"]}
        if op == "global":
            return {"summary": "One community: European landmarks and plant biology.",
                    "communities": [{"id": "c0", "summary": "landmarks + biology", "score": 1.0}]}
        return {"op": op, "note": "unimplemented"}

    return s


if __name__ == "__main__":
    srv = build()
    if len(sys.argv) >= 3 and sys.argv[1] == "--http":
        srv.serve_http(int(sys.argv[2]))
    else:
        srv.serve_stdio()
