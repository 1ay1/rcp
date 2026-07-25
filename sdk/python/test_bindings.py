"""Smoke test for the native Python RCP SDK: client + server + capability gating.

Pure standard library — no compiled module. The ``client↔C++`` case proves
cross-language interop against ``sdk/cpp/example_server`` when it is built.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import rcp


def test_server_handle_inproc():
    """Exercise the Server dispatcher in-process (no transport)."""
    s = rcp.Server()
    s.set_info("py-engine", "1.0")
    s.advertise(rcp.Capability.Retrieve, {"maxK": 100, "modes": ["hybrid"]})

    @s.on("retrieve")
    def _(params):
        return {"hits": [{"id": "d1", "score": 0.9, "text": params["query"]}]}

    # pre-initialize -> NotInitialized
    r = s.handle({"jsonrpc": "2.0", "id": 1, "method": "retrieve", "params": {"query": "x", "k": 1}})
    assert r["error"]["code"] == -32001, r

    init = s.handle({"jsonrpc": "2.0", "id": 2, "method": "initialize",
                     "params": {"protocolVersion": 1}})
    caps = init["result"]["capabilities"]
    assert "retrieve" in caps and "embed" not in caps, caps

    ret = s.handle({"jsonrpc": "2.0", "id": 3, "method": "retrieve",
                    "params": {"query": "hello", "k": 1}})
    assert ret["result"]["hits"][0]["text"] == "hello", ret

    # unadvertised capability -> CapabilityMissing
    emb = s.handle({"jsonrpc": "2.0", "id": 4, "method": "embed", "params": {"texts": ["a"]}})
    assert emb["error"]["code"] == -32003, emb

    print("server in-proc: ok")


def test_client_against_cpp_server():
    """Python client -> C++ example server subprocess (cross-language interop)."""
    cpp_server = os.path.normpath(
        os.path.join(os.path.dirname(__file__), "..", "cpp", "example_server"))
    if not os.path.exists(cpp_server):
        print("client<->C++: skipped (build sdk/cpp/example_server first)")
        return

    c = rcp.connect_stdio([cpp_server])
    assert c.protocol_version == 1
    assert c.supports(rcp.Capability.Retrieve)
    assert not c.supports(rcp.Capability.Rerank)

    hits = c.retrieve("landmark in the French capital", k=2)
    assert len(hits) == 2 and hits[0]["id"], hits
    # the C++ server now returns the agentic/eval fields — assert they survive the
    # cross-language wire and the (non-lossy) client normalisation.
    assert "confidence" in hits[0] and 0.0 <= hits[0]["confidence"] <= 1.0, hits[0]
    assert hits[0].get("unit") == "chunk", hits[0]
    assert hits[0].get("trust", {}).get("level") == "trusted", hits[0]
    print(f"client<->C++: ok, top hit = {hits[0]['id']}")

    # feedback + memory round-trip across languages against the C++ server
    if c.supports(rcp.Capability.Feedback):
        fb = c.feedback([{"hitId": hits[0]["id"], "used": True, "reward": 0.9}], query="q")
        assert fb.get("accepted") == 1, fb
        print("client<->C++ feedback: ok")
    if c.supports(rcp.Capability.Memory):
        mr = c.memory_recall("french landmark", n=2)
        assert isinstance(mr.get("clues"), list) and mr["clues"], mr
        print("client<->C++ memory: ok")

    # ping echoes the nonce (ungated, works any time)
    pong = c.ping(123)
    assert pong.get("nonce") == 123, pong
    print("ping: ok")

    # capability gating raises client-side for an unadvertised method
    try:
        c.rerank("q", ["a", "b"])
        assert False, "expected gating error"
    except RuntimeError as e:
        assert "rerank" in str(e), e
    print("client gating: ok")


def test_registry_selector():
    """Selector.loads builds from an rcp.json registry and picks a live backend."""
    here = os.path.dirname(os.path.abspath(__file__))
    srv = os.path.join(here, "..", "..", "examples", "example_server.py")
    reg = ('{ "engines": [ { "id": "docs", "transport": "stdio", '
           '"command": ["python3", "%s"], "priority": 10 } ] }' % srv)
    sel = rcp.Selector.loads(reg)
    assert sel.size == 1, sel.size
    c = sel.select_primary()
    assert c.supports(rcp.Capability.Retrieve)
    hits = c.retrieve("french landmark", k=1)
    assert len(hits) == 1, hits
    print("registry selector: ok")


def test_agentic_surfaces():
    """feedback + memory methods, capability gating, and full Hit-field round-trip."""
    s = rcp.Server()
    s.set_info("py-agentic", "1.0")
    s.advertise(rcp.Capability.Retrieve, {"maxK": 100, "units": ["chunk", "tree-node"],
                                          "tokenBudget": True, "confidence": True})
    s.advertise(rcp.Capability.Session, {"dedup": True})
    s.advertise(rcp.Capability.Feedback, {})
    s.advertise(rcp.Capability.Memory, {"scopes": ["global"], "clues": True})

    seen = {}

    @s.on("retrieve")
    def _(params):
        # echo back the agentic params so the client can assert they arrived
        seen["retrieve"] = params
        return {"hits": [{
            "id": 42, "score": 0.9, "text": params["query"],
            "confidence": 0.83, "unit": params.get("unit", "chunk"),
            "level": params.get("level", 0),
            "provenance": {"path": ["e:a", "e:b"], "leaves": ["doc:1#0"]},
            "trust": {"level": "trusted", "injectionSuspected": False},
            "scores": {"dense": 0.7, "rerank": 0.9},
        }], "usage": {"tokens": 128}}

    @s.on("feedback")
    def _(params):
        seen["feedback"] = params
        return {"accepted": len(params.get("signals", []))}

    @s.on("memory/build")
    def _(params):
        seen["memory/build"] = params
        return {"memoryId": "mem-1", "tokens": 2048}

    @s.on("memory/recall")
    def _(params):
        seen["memory/recall"] = params
        return {"clues": [{"query": "sub-q", "seedIds": ["e:a"], "weight": 0.9}]}

    # in-proc round-trip through a stdio pipe pair would be heavier; drive the
    # dispatcher directly, mirroring test_server_handle_inproc.
    s.handle({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": 1}})

    # retrieve with agentic knobs; assert new hit fields survive normalisation
    from rcp._client import _hit_to_dict
    ret = s.handle({"jsonrpc": "2.0", "id": 2, "method": "retrieve",
                    "params": {"query": "q", "k": 1, "unit": "tree-node",
                               "level": 2, "tokenBudget": 500, "sessionId": "traj-1"}})
    assert seen["retrieve"]["unit"] == "tree-node"
    assert seen["retrieve"]["sessionId"] == "traj-1"
    hit = _hit_to_dict(ret["result"]["hits"][0])
    assert hit["id"] == "42" and hit["confidence"] == 0.83
    assert hit["unit"] == "tree-node" and hit["level"] == 2
    assert hit["provenance"]["path"] == ["e:a", "e:b"]
    assert hit["trust"]["level"] == "trusted"
    assert hit["scores"]["dense"] == 0.7

    # feedback
    fb = s.handle({"jsonrpc": "2.0", "id": 3, "method": "feedback",
                   "params": {"sessionId": "traj-1", "query": "q",
                              "signals": [{"hitId": "42", "used": True, "cited": True, "reward": 0.8}]}})
    assert fb["result"]["accepted"] == 1, fb

    # memory build + recall
    mb = s.handle({"jsonrpc": "2.0", "id": 4, "method": "memory/build",
                   "params": {"scope": "global"}})
    assert mb["result"]["memoryId"] == "mem-1", mb
    mr = s.handle({"jsonrpc": "2.0", "id": 5, "method": "memory/recall",
                   "params": {"query": "q", "memoryId": "mem-1", "n": 3}})
    assert mr["result"]["clues"][0]["seedIds"] == ["e:a"], mr

    # a server that never advertised memory rejects it with CapabilityMissing
    s2 = rcp.Server()
    s2.set_info("bare", "1.0")
    s2.advertise(rcp.Capability.Retrieve, {})
    s2.handle({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": 1}})
    miss = s2.handle({"jsonrpc": "2.0", "id": 2, "method": "memory/recall", "params": {"query": "q"}})
    assert miss["error"]["code"] == -32003, miss

    print("agentic surfaces (feedback/memory/hit-fields): ok")


def test_filter():
    from rcp import filter as f
    # Builder emits the exact spec §8 wire shape.
    assert f.eq("lang", "fr").to_json() == {"field": "lang", "op": "eq", "value": "fr"}
    tree = f.all_(f.eq("lang", "en"), f.gte("year", 2015)).to_json()
    assert tree == {"and": [
        {"field": "lang", "op": "eq", "value": "en"},
        {"field": "year", "op": "gte", "value": 2015},
    ]}
    # Operator overloads compose to the same tree.
    assert (f.eq("lang", "en") & f.gte("year", 2015)).to_json() == tree

    fields = {"year": "int", "lang": "keyword"}
    assert f.validate(tree, fields=fields) == tree
    assert f.validate(None, fields=fields) is None
    assert f.validate({}, fields=fields) is None

    bad = [
        {"field": "author", "op": "eq", "value": "z"},
        {"field": "lang", "op": "equals", "value": "en"},
        {"and": "not-a-list"},
        {"field": "year", "op": "in", "value": 2017},
        {"or": [{"field": "lang"}]},
        {"field": "lang", "op": "gt", "value": "en"},
        {"field": "x", "op": "eq", "value": 1, "and": []},
    ]
    for b in bad:
        try:
            f.validate(b, fields=fields)
            raise AssertionError(f"expected -32602 for {b}")
        except rcp.RcpError as e:
            assert e.code == rcp.Errc.INVALID_PARAMS
            assert isinstance(e.data, dict) and "field" in e.data

    # Builder rejects nonsense locally.
    for ctor in (lambda: f.eq("", "x"), lambda: f.all_("nope")):
        try:
            ctor()
            raise AssertionError("builder should reject")
        except (ValueError, TypeError):
            pass
    print("filter builder + validator: ok")


def test_fusion():
    # RRF matches the hand computation (rrf_k=60), spec §16.3.
    A = [{"id": "a"}, {"id": "b"}, {"id": "c"}]   # ranks 1,2,3
    B = [{"id": "b"}, {"id": "d"}]                # ranks 1,2
    order = [h["id"] for h in rcp.rrf_fuse({"e1": A, "e2": B}, rrf_k=60)]
    # b=1/61+1/61 > a=1/61 > d=1/62 > c=1/63
    assert order == ["b", "a", "d", "c"], order

    # k truncation + origin tagging.
    top2 = rcp.rrf_fuse({"e1": A, "e2": B}, k=2)
    assert [h["id"] for h in top2] == ["b", "a"]
    assert top2[0]["meta"]["engine"] in ("e1", "e2")
    assert "fusedScore" in top2[0]["meta"]

    # Deterministic tie-break: equal fused scores -> id ascending.
    tie = rcp.rrf_fuse({"e1": [{"id": "y"}], "e2": [{"id": "x"}]})
    assert [h["id"] for h in tie] == ["x", "y"], tie

    # Weighted fusion normalises per engine before summing.
    wa = [{"id": "a", "score": 10}, {"id": "b", "score": 5}]  # a->1, b->0
    wb = [{"id": "b", "score": 8}, {"id": "c", "score": 2}]   # b->1, c->0
    worder = [h["id"] for h in rcp.weighted_fuse({"e1": wa, "e2": wb})]
    assert worder[0] in ("a", "b") and "c" == worder[-1], worder

    # Dedup keeps the richest body across engines.
    rich = rcp.rrf_fuse({"e1": [{"id": "x", "text": "short"}],
                         "e2": [{"id": "x", "text": "a much longer body"}]})
    assert rich[0]["text"] == "a much longer body"
    print("federation fusion (RRF + weighted): ok")


def test_vector_codec():
    v = [[1.0, 2.0, 3.5], [0.5, -1.0, 2.25]]
    enc, meta = rcp.encode_vectors(v, "f32-base64")
    assert meta == {"encoding": "f32-base64", "dimension": 3}
    assert rcp.decode_vectors(enc, **meta) == v          # exact round trip
    j, jmeta = rcp.encode_vectors(v)
    assert jmeta == {} and rcp.decode_vectors(j) == v    # json default
    try:
        rcp.decode_vectors(enc, encoding="f32-base64", dimension=99)
        raise AssertionError("expected a dimension mismatch to raise")
    except ValueError:
        pass
    print("f32-base64 vector codec: ok")


if __name__ == "__main__":
    test_server_handle_inproc()
    test_client_against_cpp_server()
    test_registry_selector()
    test_agentic_surfaces()
    test_filter()
    test_fusion()
    test_vector_codec()
    print("\nall native Python SDK checks passed")
