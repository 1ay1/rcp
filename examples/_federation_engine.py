#!/usr/bin/env python3
"""_federation_engine.py — a tiny stdio RCP engine for the federation demo.

Not meant to be run by hand: `example_federation.py` spawns two copies of this,
each seeded with a DIFFERENT corpus (via argv), so the demo has two genuinely
independent engines to fan out to and fuse. Each is a real RCP server speaking
the wire protocol over stdin/stdout.
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "sdk" / "python"))

import rcp  # noqa: E402

# Each engine's corpus is passed as `id::text` pairs on the command line.
CORPORA = {
    "papers": [
        ("p-splade", "SPLADE learns sparse lexical expansions for retrieval."),
        ("p-colbert", "ColBERT does late interaction over token embeddings."),
        ("p-rrf", "Reciprocal Rank Fusion merges rankings without score calibration."),
        ("p-dpr", "Dense Passage Retrieval encodes queries and passages into vectors."),
    ],
    "web": [
        ("w-rrf", "RRF is a simple, robust way to combine search engine results."),
        ("w-hybrid", "Hybrid search blends dense vectors with sparse keyword signals."),
        ("w-rerank", "Cross-encoder rerankers rescore the top candidates precisely."),
        ("w-bm25", "BM25 is the classic sparse lexical ranking function."),
    ],
}


def _overlap(query: str, text: str) -> float:
    q = set(query.lower().replace(".", "").split())
    t = set(text.lower().replace(".", "").split())
    return len(q & t) / (len(q) or 1)


def main() -> int:
    which = sys.argv[1] if len(sys.argv) > 1 else "papers"
    docs = CORPORA[which]

    s = rcp.Server()
    s.set_info(f"engine-{which}", "1.0")
    s.advertise(rcp.Capability.Retrieve, {"maxK": 100, "modes": ["hybrid"]})

    def retrieve(params):
        query = params.get("query", "")
        k = params.get("k", 5)
        scored = sorted(
            ({"id": did, "text": text, "score": _overlap(query, text)} for did, text in docs),
            key=lambda h: -h["score"],
        )
        return {"hits": [h for h in scored if h["score"] > 0][:k]}

    s.on("retrieve", retrieve)
    s.serve_stdio()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
