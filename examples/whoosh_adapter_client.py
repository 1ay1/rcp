#!/usr/bin/env python3
"""Drive the Whoosh-backed RCP adapter over stdio and assert real behaviour.

This is the *client half* of the leak test. It proves an ordinary RCP client
(no knowledge that Whoosh is behind the wire) gets correct, structured results:
BM25 relevance ranking, a boolean filter tree honoured by a real query engine,
snippet citations, and provenance — all through the unmodified protocol.

Run: examples/.poc-venv/bin/python examples/whoosh_adapter_client.py
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "sdk", "python"))
import rcp  # noqa: E402

PY = sys.executable
SERVER = [PY, os.path.join(HERE, "whoosh_adapter.py")]


def main():
    c = rcp.connect_stdio(SERVER)
    print("connected to:", c._server)
    caps = c.capabilities
    assert "retrieve" in caps, caps
    assert "filter" in caps, "filter capability must be advertised"
    assert c.supports(rcp.Capability.Filter), "typed enum gating must work now"
    assert caps["retrieve"]["modes"] == ["sparse"], caps["retrieve"]
    print("caps OK:", sorted(caps))

    # 1. Real BM25 ranking: 'eiffel tower paris' should surface d1 (en) top.
    hits = c.retrieve("eiffel tower paris", k=3)
    assert hits, "expected hits"
    top = hits[0]
    assert top["id"] == "d1", top
    assert top["score"] > 0
    assert 0.0 <= top["confidence"] <= 1.0, top
    assert top["unit"] == "passage"
    assert top["provenance"]["lang"] == "en"
    assert "citation" in top and top["citation"]["quote"], top
    assert top["scores"]["bm25f"] == top["score"]
    print(f"rank OK: top={top['id']} score={top['score']:.3f} "
          f"conf={top['confidence']:.3f} quote={top['citation']['quote'][:48]!r}")

    # 2. Filter tree (real spec §8 shape) honoured by Whoosh's query engine:
    #    French-only should now return d5, not the English d1. Built two ways —
    #    a raw dict AND the typed rcp.filter builder — which must agree exactly.
    raw_flt = {"field": "lang", "op": "eq", "value": "fr"}
    built_flt = rcp.filter.eq("lang", "fr").to_json()
    assert built_flt == raw_flt, (built_flt, raw_flt)
    fr = c.retrieve("eiffel tour paris", k=3, opts={"filter": built_flt})
    assert fr and all(h["provenance"]["lang"] == "fr" for h in fr), fr
    assert fr[0]["id"] == "d5", fr
    print("filter eq OK (builder==raw): fr ->", [h["id"] for h in fr])

    # 3. Nested boolean + range filter via the builder's operator overloads.
    recent_flt = rcp.filter.all_(
        rcp.filter.eq("lang", "en"),
        rcp.filter.gte("year", 2015),
    )
    recent = c.retrieve("model attention retrieval", k=5,
                        opts={"filter": recent_flt.to_json()})
    ids = {h["id"] for h in recent}
    assert ids and ids <= {"d4", "d6"}, ids  # d4=2017, d6=2020 are the only matches
    assert all(h["provenance"]["year"] >= 2015 for h in recent), recent
    print("filter and+range OK: ->", sorted(ids))

    # 4. Robustness: every malformed / unauthorized filter -> clean -32602,
    #    never a -32603 Internal crash. This is the leak that used to exist.
    bad_filters = [
        {"field": "author", "op": "eq", "value": "z"},   # unadvertised field
        {"field": "lang", "op": "equals", "value": "en"}, # typo'd operator
        {"and": "not-a-list"},                             # malformed combinator
        {"field": "year", "op": "in", "value": 2017},      # in wants an array
        {"or": [{"field": "lang"}]},                       # leaf missing op
        {"field": "lang", "op": "gt", "value": "en"},      # ordered op on keyword
    ]
    for bad in bad_filters:
        try:
            c.retrieve("x", k=1, opts={"filter": bad})
            raise AssertionError(f"expected -32602 for {bad}")
        except rcp.RcpError as e:
            assert e.code == rcp.Errc.INVALID_PARAMS, (bad, e.code)
            assert isinstance(e.data, dict) and "field" in e.data, (bad, e.data)
    print(f"filter reject OK: all {len(bad_filters)} malformed filters -> -32602")

    # 5. The builder itself rejects nonsense locally (before the wire).
    for construct in (
        lambda: rcp.filter.eq("", "x"),          # empty field
        lambda: rcp.filter._leaf("f", "equals"), # unknown op
        lambda: rcp.filter.all_("not-a-filter"), # non-Filter clause
    ):
        try:
            construct()
            raise AssertionError("builder should have rejected bad input")
        except (ValueError, TypeError):
            pass
    print("builder local validation OK")

    c.shutdown()
    print("\nALL PASS — Whoosh drives RCP with zero spec changes.")


if __name__ == "__main__":
    main()
