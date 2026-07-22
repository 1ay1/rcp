# rcp — Retrieval Context Protocol, Node.js SDK

A **native, zero-dependency** Node.js SDK for [RCP](https://1ay1.github.io/rcp/) —
the open, versioned JSON-RPC protocol that lets any RAG engine expose
`embed` / `rerank` / `retrieve` / `graph` / `index` / `catalog`, and any client
consume it uniformly.

Standard library only (`child_process`, `http`, `net`, `readline`) — no compiler,
no native addon, nothing to build. It speaks the exact same wire format as the
[C++](../cpp) and [Python](../python) SDKs, so a Node client and a C++/Python
server (or vice-versa) interoperate byte-for-byte.

## Install

```sh
npm install rcp-protocol
```

Requires Node ≥ 18. The package is ESM (`"type": "module"`).

## Client

Connect to any RCP server over a subprocess (stdio) or HTTP, then make typed,
capability-gated calls. Every call returns a Promise.

```js
import * as rcp from "rcp-protocol";

const c = await rcp.connectStdio(["node", "my_server.js"]);
// or: const c = await rcp.connectHttp("http://127.0.0.1:8000/rcp");

console.log(c.server, c.protocolVersion, Object.keys(c.capabilities));

if (c.supports(rcp.Capability.Retrieve)) {
  for (const hit of await c.retrieve("eiffel tower", 3)) {
    console.log(hit.id, hit.score, hit.text);
  }
}

if (c.supports(rcp.Capability.Embed)) {
  const [vec] = await c.embed(["hello world"]);
  console.log("dim", vec.length);
}

await c.shutdown();
```

A call to a capability the server never advertised throws **before** any I/O:

```js
try {
  await c.rerank("q", ["a", "b"]);
} catch (e) {
  // e instanceof rcp.RcpError, e.code === rcp.Errc.CAPABILITY_MISSING
  // e.message === "[RCP -32003] server does not advertise 'rerank'"
}
```

Client methods: `embed`, `embedSparse`, `embedMulti`, `rerank`, `retrieve`,
`search` (returns `{ hits, usage, nextCursor }`), `graph`, `transform`,
`indexAdd`, `indexDelete`, `catalog`, `info`, `ping`, `call`, `shutdown`.

## Server

Expose a Node RAG engine as an RCP server. Handlers may be sync or async.

```js
import * as rcp from "rcp-protocol";

const s = new rcp.Server();
s.setInfo("my-engine", "1.0");
s.advertise(rcp.Capability.Retrieve, { maxK: 100, modes: ["hybrid"] });

s.on(rcp.Method.RETRIEVE, async (params) => {
  const hits = await myIndex.search(params.query, params.k ?? 10);
  return { hits: hits.map((h) => ({ id: h.id, score: h.score, text: h.text })) };
});

await s.serveStdio();          // or: await s.serveHttp(8000);
```

The `Server` owns the `initialize` handshake, capability gating, JSON-RPC framing
and batching, and error mapping. It answers `initialize` / `info` / `ping` /
`shutdown` itself; a gated call before `initialize` is `-32001`, an unadvertised
or unimplemented method is `-32003`, an unknown method is `-32004`.

## Selecting a backend

Pick one engine from a registry — by id, by required capability, or by priority
with liveness fallback. Connection is lazy.

```js
const sel = rcp.Selector.loads(JSON.stringify({
  engines: [
    { id: "docs", transport: "stdio", command: ["node", "server.js"], priority: 10 },
    { id: "web", transport: "http", url: "http://127.0.0.1:8000/rcp", priority: 5 },
  ],
}));

const c = await sel.selectCapable(rcp.Capability.Retrieve);
```

## Examples & tests

```sh
node examples/example_server.js            # stdio (default)
node examples/example_server.js --http 8000
node examples/example_client.js            # drives the example server

node test.js                               # smoke test incl. Node client ↔ C++ server
```

## License

MIT © 2025 Ayush Bhat.
