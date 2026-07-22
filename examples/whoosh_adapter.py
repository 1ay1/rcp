#!/usr/bin/env python3
"""RCP/1 server backed by a REAL third-party retrieval engine (Whoosh).

This is a *leak test* for the RCP abstraction, not a toy. Unlike
`example_server.py` (a hand-rolled index authored by the RCP maintainers), every
retrieval decision here is delegated to Whoosh — a full-text search library with
an inverted index, a real BM25F scorer, a real query parser, snippet
highlighting, term extraction, and more-like-this, none of whose API the RCP
authors designed or control. If RCP is the right shape, mapping Whoosh onto it
should feel natural and require *zero* changes to the spec. Where it does not,
the mismatch is recorded in `whoosh_adapter_LEAKS.md`.

Run (stdio):   examples/.poc-venv/bin/python examples/whoosh_adapter.py
Run (http):    examples/.poc-venv/bin/python examples/whoosh_adapter.py --http 8000

Requires: pip install Whoosh   (see examples/.poc-venv)
"""
import math
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "sdk", "python"))
import rcp  # noqa: E402

from whoosh import scoring, highlight  # noqa: E402
from whoosh.fields import Schema, TEXT, ID, NUMERIC, KEYWORD  # noqa: E402
from whoosh.qparser import QueryParser, OrGroup  # noqa: E402
from whoosh.query import And, Or, Not, Term, NumericRange  # noqa: E402
from whoosh.filedb.filestore import RamStorage  # noqa: E402

# --- A small, real corpus with structured metadata (year, lang) so we can
#     exercise RCP's filter surface against Whoosh's actual query engine. ---
CORPUS = [
    ("d1", "The Eiffel Tower is a wrought-iron lattice tower on the Champ de Mars in Paris, France. It is named after engineer Gustave Eiffel.", 1889, "en"),
    ("d2", "Photosynthesis is the process by which plants convert light energy into chemical energy stored in glucose, releasing oxygen.", 2001, "en"),
    ("d3", "The Great Wall of China is a series of fortifications built across the historical northern borders of ancient Chinese states.", 1990, "en"),
    ("d4", "Transformer models use self-attention to weigh the relevance of tokens in a sequence, enabling parallel training over long contexts.", 2017, "en"),
    ("d5", "La tour Eiffel est une tour de fer puddle situee a Paris. Elle est le monument le plus visite au monde.", 1889, "fr"),
    ("d6", "Retrieval-augmented generation grounds a language model on documents fetched from an external index at inference time.", 2020, "en"),
]


def _build_index():
    schema = Schema(
        uri=ID(stored=True, unique=True),
        body=TEXT(stored=True),           # stored so we can return + highlight
        year=NUMERIC(stored=True),
        lang=KEYWORD(stored=True),
    )
    ix = RamStorage().create_index(schema)
    w = ix.writer()
    for uri, body, year, lang in CORPUS:
        w.add_document(uri=uri, body=body, year=year, lang=lang)
    w.commit()
    return ix


IX = _build_index()
# A real, tunable BM25F scorer — the same knobs the spec exposes as capability.
WEIGHTING = scoring.BM25F(B=0.75, K1=1.2)

# Advertised filter surface (spec §8). The SDK validator gates against this, so
# an unauthorized field/op becomes a clean -32602 before Whoosh is ever touched.
_FILTER_FIELDS = {"year": "int", "lang": "keyword"}
_FILTER_OPS = ["eq", "ne", "gt", "gte", "lt", "lte", "in", "nin"]


def _leaf(field, op, value):
    """Compile ONE validated leaf into a Whoosh query. The tree is already
    validated by rcp.filter.validate(), so no defensive parsing is needed here."""
    if op == "eq":
        return Term(field, str(value))
    if op == "ne":
        return Not(Term(field, str(value)))
    if op in ("gt", "gte", "lt", "lte"):
        lo = value if op in ("gt", "gte") else None
        hi = value if op in ("lt", "lte") else None
        return NumericRange(field, lo, hi,
                            startexcl=(op == "gt"), endexcl=(op == "lt"))
    if op == "in":
        return Or([Term(field, str(v)) for v in value])
    if op == "nin":
        return Not(Or([Term(field, str(v)) for v in value]))
    raise AssertionError(f"unreachable op {op!r}")  # validator guarantees coverage


def _compile_filter(node):
    """Compile a VALIDATED RCP filter tree (spec §8) into a Whoosh query.

    Input is trusted: it has already passed rcp.filter.validate(), so every
    combinator/leaf is well-formed and every field/op is advertised. This keeps
    the backend-specific code tiny and total.
    """
    if node is None:
        return None
    if "and" in node:
        return And([_compile_filter(c) for c in node["and"]])
    if "or" in node:
        return Or([_compile_filter(c) for c in node["or"]])
    if "not" in node:
        return Not(_compile_filter(node["not"]))
    return _leaf(node["field"], node["op"], node["value"])


def _normalize_score(raw, top):
    """Whoosh BM25F scores are unbounded and corpus-relative. RCP `score` is
    'higher is better' on a server-declared scale; `confidence` is [0,1]. We
    declare the scale as raw-bm25 and derive a bounded confidence by squashing.
    """
    conf = 1.0 - math.exp(-raw / 4.0) if raw > 0 else 0.0
    return raw, round(conf, 4)


def _search(query, k, rcp_filter=None, want_snippets=True):
    with IX.searcher(weighting=WEIGHTING) as s:
        parser = QueryParser("body", schema=IX.schema, group=OrGroup)
        q = parser.parse(query or "*")
        # Validate against the advertised surface (raises -32602 on bad input),
        # then compile the now-trusted tree to a Whoosh query.
        clean = rcp.filter.validate(rcp_filter, fields=_FILTER_FIELDS, operators=_FILTER_OPS)
        mask = _compile_filter(clean)
        results = s.search(q, limit=max(k, 1), filter=mask)
        results.fragmenter = highlight.ContextFragmenter(maxchars=160, surround=40)
        top = results[0].score if len(results) else 1.0
        hits = []
        for r in results:
            raw, conf = _normalize_score(r.score, top)
            hit = {
                "id": r["uri"],
                "score": raw,
                "confidence": conf,
                "text": r["body"],
                "unit": "passage",
                # Provenance straight from the engine's stored fields.
                "provenance": {"source": r["uri"], "year": r["year"], "lang": r["lang"]},
                # Per-stage scores: RCP lets a hit expose which stage produced
                # which number. Whoosh only ran BM25F, so that's what we report.
                "scores": {"bm25f": raw},
            }
            if want_snippets:
                # Whoosh generates a real highlighted snippet from term positions;
                # this is a natural citation.quote.
                snippet = r.highlights("body", top=1)
                if snippet:
                    hit["citation"] = {"source": r["uri"], "quote": snippet}
            hits.append(hit)
        return hits


def build():
    srv = rcp.Server()
    srv.set_info("rcp-whoosh-adapter", "1.0.0")
    # We advertise ONLY what Whoosh can actually back. Whoosh is a sparse
    # (lexical/BM25) engine: no dense vectors, so we advertise mode "sparse".
    srv.advertise(rcp.Capability.Retrieve, {
        "maxK": 100,
        "modes": ["sparse"],
        "citations": True,
        "scoreScale": "unbounded",  # raw BM25F is engine-relative (spec §6.1)
    })
    # `filter` is now a first-class Capability enum member (added to all four
    # SDKs as a direct result of this leak test — see whoosh_adapter_LEAKS.md #2).
    srv.advertise(rcp.Capability.Filter, {
        "fields": {"year": "int", "lang": "keyword"},
        "operators": ["eq", "ne", "gt", "gte", "lt", "lte", "in", "nin"],
    })

    @srv.on(rcp.Method.RETRIEVE)
    def _retrieve(params):
        # rcp.filter.validate() inside _search raises RcpError(-32602) directly
        # for a malformed or unauthorized filter — the server needs no try/except
        # and can never leak a raw KeyError as -32603.
        return {"hits": _search(
            params.get("query", ""),
            int(params.get("k", 10)),
            params.get("filter"),
        )}

    return srv


if __name__ == "__main__":
    s = build()
    if len(sys.argv) >= 3 and sys.argv[1] == "--http":
        s.serve_http(int(sys.argv[2]))
    else:
        s.serve_stdio()
