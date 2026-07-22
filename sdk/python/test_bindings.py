"""Smoke test for the Python RCP bindings: client + server + typestate gating."""
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
    print(f"client<->C++: ok, top hit = {hits[0]['id']}")

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


if __name__ == "__main__":
    test_server_handle_inproc()
    test_client_against_cpp_server()
    test_registry_selector()
    print("\nall python binding checks passed")
