# rcp — Retrieval Context Protocol, Python SDK

A **native, pure-standard-library** Python SDK for
[RCP](https://rcp-6d6ef6d5.mintlify.site/) — the open, versioned JSON-RPC
protocol that lets any RAG engine expose
`embed` / `rerank` / `retrieve` / `graph` / `index` / `catalog`, and any client
consume it uniformly.

No dependencies, no compiler, no build step — just `subprocess`, `http`, `socket`,
and `json` from the standard library. It speaks the exact same wire format as the
[C++](../cpp), [Node.js](../node), and [Rust](../rust) SDKs, so a Python client
and a C++/Node/Rust server (or vice-versa) interoperate byte-for-byte.

## Install

```sh
pip install rcp-protocol
```

Requires Python ≥ 3.9. The import name is `rcp`:

```python
import rcp
```

## Client

Connect to any RCP server over a subprocess (stdio) or HTTP, then make typed,
capability-gated calls.

```python
import rcp

c = rcp.connect_stdio(["python3", "my_server.py"])
# or: c = rcp.connect_http("http://127.0.0.1:8000/rcp")

print(c.server(), c.protocol_version(), list(c.capabilities()))

if c.supports(rcp.Capability.Retrieve):
    for hit in c.retrieve("eiffel tower", k=3):
        print(hit["id"], hit["score"], hit["text"])

if c.supports(rcp.Capability.Embed):
    (vec,) = c.embed(["hello world"])
    print("dim", len(vec))

c.shutdown()
```

A call to a capability the server never advertised raises **before** any I/O:

```python
try:
    c.rerank("q", ["a", "b"])
except rcp.RcpError as e:
    # e.code == rcp.Errc.CAPABILITY_MISSING  (-32003)
    ...
```

Client methods: `embed`, `embed_sparse`, `embed_multi`, `rerank`, `retrieve`,
`search` (returns `hits` + `usage` + `nextCursor`), `graph`, `transform`,
`index_add`, `index_delete`, `catalog`, `info`, `ping`, `call`, `shutdown`.

## Server

Expose a Python RAG engine as an RCP server.

```python
import rcp

s = rcp.Server()
s.set_info("my-engine", "1.0")
s.advertise(rcp.Capability.Retrieve, {"maxK": 100, "modes": ["hybrid"]})

@s.on(rcp.Method.RETRIEVE)
def _(params):
    hits = my_index.search(params["query"], params.get("k", 10))
    return {"hits": [{"id": h.id, "score": h.score, "text": h.text} for h in hits]}

s.serve_stdio()          # or: s.serve_http(8000)
```

The `Server` owns the `initialize` handshake, capability gating, JSON-RPC framing
and batching, and error mapping. It answers `initialize` / `info` / `ping` /
`shutdown` itself; a gated call before `initialize` is `-32001`, an unadvertised
or unimplemented method is `-32003`, an unknown method is `-32004`.

## Selecting a backend

Pick one engine from a registry — by id, by required capability, or by priority
with liveness fallback. Connection is lazy.

```python
sel = rcp.Selector.loads('''{
  "engines": [
    {"id": "docs", "transport": "stdio", "command": ["python3", "server.py"], "priority": 10},
    {"id": "web",  "transport": "http",  "url": "http://127.0.0.1:8000/rcp",   "priority": 5}
  ]
}''')

c = sel.select_capable(rcp.Capability.Retrieve)
```

## Examples & tests

```sh
python3 examples/example_server.py            # stdio (default)
python3 examples/example_server.py --http 8000
python3 examples/example_client.py            # drives the example server

python3 test_bindings.py                      # smoke test incl. Python client ↔ C++ server
```

## License

MIT © 2026 Ayush Bhat.
