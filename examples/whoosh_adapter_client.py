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
    #    French-only should now return d5, not the English d1.
    fr = c.retrieve("eiffel tour paris", k=3,
                    opts={"filter": {"field": "lang", "op": "eq", "value": "fr"}})
    assert fr and all(h["provenance"]["lang"] == "fr" for h in fr), fr
    assert fr[0]["id"] == "d5", fr
    print("filter eq OK: fr ->", [h["id"] for h in fr])

    # 3. Nested boolean + range filter: recent (year >= 2015) English docs.
    recent = c.retrieve("model attention retrieval", k=5, opts={"filter": {
        "and": [
            {"field": "lang", "op": "eq", "value": "en"},
            {"field": "year", "op": "gte", "value": 2015},
        ]}})
    ids = {h["id"] for h in recent}
    assert ids and ids <= {"d4", "d6"}, ids  # d4=2017, d6=2020 are the only matches
    assert all(h["provenance"]["year"] >= 2015 for h in recent), recent
    print("filter and+range OK: ->", sorted(ids))

    # 4. Unadvertised filter field -> RCP -32602 (spec §8), not a crash.
    try:
        c.retrieve("x", k=1, opts={"filter": {"field": "author", "op": "eq", "value": "z"}})
        raise AssertionError("expected -32602 for unadvertised field")
    except rcp.RcpError as e:
        assert e.code == rcp.Errc.INVALID_PARAMS, e.code
        print("filter reject OK: -32602 for unadvertised field")

    c.shutdown()
    print("\nALL PASS — Whoosh drives RCP with zero spec changes.")


if __name__ == "__main__":
    main()
