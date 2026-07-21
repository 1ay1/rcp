#!/usr/bin/env python3
"""Example RCP/1 server — a complete, dependency-free retrieval engine.

Run as a subprocess (stdio, the default):
    python3 examples/example_server.py
Or over HTTP:
    python3 examples/example_server.py --http 8000

It advertises embed + retrieve + graph over a toy in-memory index. Swap the
handler bodies for sentence-transformers / FAISS / networkx / an LLM — the wire
contract (JSON-RPC over stdio or HTTP) is all a client ever sees.
"""
import hashlib
import math
import os
import sys

# Make the sibling SDK importable when run from the repo without installing.
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "sdk", "python"))

import rcp  # noqa: E402

DIM = 384
DOCS = [
    ("d1", "The Eiffel Tower is a wrought-iron lattice tower in Paris, France."),
    ("d2", "Photosynthesis converts light energy into chemical energy in plants."),
    ("d3", "The Great Wall of China stretches thousands of kilometres."),
]


def embed_one(text):
    v = [0.0] * DIM
    for tok in text.lower().split():
        h = int(hashlib.blake2b(tok.encode(), digest_size=8).hexdigest(), 16)
        v[h % DIM] += 1.0
    n = math.sqrt(sum(x * x for x in v)) or 1.0
    return [x / n for x in v]


_INDEX = [(uri, text, embed_one(text)) for uri, text in DOCS]


def _cosine(a, b):
    return sum(x * y for x, y in zip(a, b))


def _search(query, k):
    q = embed_one(query)
    scored = sorted(((_cosine(q, vec), uri, text) for uri, text, vec in _INDEX),
                    key=lambda t: t[0], reverse=True)
    return [{"id": uri, "score": s, "text": text} for s, uri, text in scored[:k]]


def build():
    s = rcp.Server()
    s.set_info("rcp-example", "1.0.0")
    s.advertise(rcp.Capability.Embed, {"dimensions": DIM, "modes": ["dense"]})
    s.advertise(rcp.Capability.Retrieve, {"maxK": 100, "modes": ["dense"], "citations": True})
    s.advertise(rcp.Capability.Graph, {"ops": ["local", "global"]})

    @s.on(rcp.Method.EMBED)
    def _embed(params):
        return {"vectors": [embed_one(t) for t in params.get("texts", [])], "dimensions": DIM}

    @s.on(rcp.Method.RETRIEVE)
    def _retrieve(params):
        return {"hits": _search(params.get("query", ""), int(params.get("k", 10)))}

    @s.on(rcp.Method.GRAPH)
    def _graph(params):
        op = params.get("op", "local")
        if op == "local":
            return {"hits": _search(params.get("query", ""), int(params.get("k", 5)))}
        if op == "global":
            return {"summary": "a global community summary would go here", "communities": 1}
        return {"op": op, "note": "unimplemented"}

    return s


if __name__ == "__main__":
    srv = build()
    if len(sys.argv) >= 3 and sys.argv[1] == "--http":
        srv.serve_http(int(sys.argv[2]))
    else:
        srv.serve_stdio()
