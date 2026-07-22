<div align="center">

# RCP — the Retrieval Context Protocol

**One open protocol so any RAG engine — any language, any vendor — can share its power, and any client can consume it uniformly.**

The retrieval companion to [MCP](https://modelcontextprotocol.io) (tools) and [ACP](https://agentclientprotocol.com) (agents).

[**Specification**](spec/rcp-1.0.md) · [**JSON Schema**](schema/rcp-1.0.json) · [**Docs**](docs) · [**C++ SDK**](sdk/cpp) · [**Python SDK**](sdk/python) · [**Node SDK**](sdk/node) · [**Rust SDK**](sdk/rust)

`RCP/1` · JSON-RPC 2.0 · MIT

</div>

---

## Why

Every RAG stack reinvents the same wire: embed, rerank, retrieve, filter, cite.
Swapping a vector DB, adding a reranker, or wiring a second knowledge base means
rewriting glue. MCP standardised *tools*; ACP standardised *agents*; **nothing
standardised the retrieval layer that grounds them.** RCP is that layer — a
small, versioned, JSON-RPC contract a *powerful* engine can fully express and a
thin client can fully consume.

- **Capability-negotiated.** A server advertises exactly what it can do
  (`embed`, `sparseEmbed`, `multiVector`, `rerank`, `retrieve`, `transform`,
  `graph`, `index`, `catalog`). Clients gate calls on capabilities — no probing,
  no surprises.
- **SOTA-complete.** Hybrid (dense + learned-sparse) search, ColBERT
  late-interaction, SPLADE, multi-stage rerank cascades, MMR diversification,
  contextual retrieval, GraphRAG (local/global/drift), agentic multi-hop,
  citations, streaming, and metadata filtering are all first-class. See
  [Appendix B](spec/rcp-1.0.md#appendix-b--mapping-sota-retrieval-to-rcp).
- **Transport-free.** JSON-RPC 2.0 over newline-delimited stdio *or* HTTP. If it
  can read a line and parse JSON, it can speak RCP.
- **Composable.** A registry (`rcp.json`) or a `catalog`-capable aggregator lets
  a client target one backend (**Selector**) or fuse many (**Federation**).

## Architecture

An agent runtime speaks **ACP** to its client, calls tools over **MCP**, and
fetches grounding context over **RCP**. All three share JSON-RPC framing, an
`initialize`/capabilities handshake, `_meta` extensibility, and cursor
pagination — learn one, you know the shape of all three.

```
  client  ⇄  ACP  ⇄  agent  ⇄  MCP  ⇄  tools
                       │
                       └──────  RCP  ⇄  retrieval engine(s)
```

## SDKs

RCP ships **four SDKs** that speak the identical wire format: a **type-theoretic
C++ SDK** (header-only), a **native Python SDK**, a **native Node.js SDK**, and a
**native Rust SDK** — the Python, Node, and Rust SDKs are dependency-free (the
last three need no external crates or packages at all). Cross-language interop is
proven by the test suites: every client drives every server, in any combination.

```sh
pip install rcp-protocol      # Python  — imports as `import rcp`
npm install rcp-protocol      # Node.js
cargo add rcp-protocol        # Rust    — crate is `rcp`
# C++ (header-only, C++23): find_package(rcp CONFIG REQUIRED) + rcp::rcp
```

The C++ SDK pushes protocol invariants into the type system, proved at **compile
time**:

- **Strong scalars & refinement types** — `TopK` cannot be `0`, `ProtocolVersion`
  is validated at construction, `Score`/`Dimension` are not interchangeable.
- **Typestate `Client`** — only constructible via a `connect*()` that runs the
  handshake; capability-gated calls fail *client-side* with `CapabilityMissing`
  before any I/O.
- **Concept-gated `Server<H>`** — a handler advertises capabilities and
  implements matching hooks; `if constexpr` dispatch means an advertised-but-
  unimplemented method is a typed error, never a crash.
- **`Result<T> = std::expected<T, Error>`** everywhere — no exceptions for
  control flow. `test_types.cpp` carries `static_assert` proofs; **the build is
  the test runner.**

### Python

```python
import rcp

# ── server ──────────────────────────────────────────────
s = rcp.Server()
s.set_info("my-engine", "1.0")
s.advertise(rcp.Capability.Retrieve, {"maxK": 100, "modes": ["hybrid"]})

@s.on(rcp.Method.RETRIEVE)
def _(params):
    hits = my_index.search(params["query"], params.get("k", 10))
    return {"hits": [{"id": h.id, "score": h.score, "text": h.text} for h in hits]}

s.serve_stdio()

# ── client ──────────────────────────────────────────────
c = rcp.connect_stdio(["python3", "my_engine.py"])
if c.supports(rcp.Capability.Retrieve):
    for h in c.retrieve("eiffel tower", k=3):
        print(h["id"], h["score"])
```

### Node.js

```js
import * as rcp from "rcp-protocol";

// ── server ──────────────────────────────────────────────
const s = new rcp.Server();
s.setInfo("my-engine", "1.0");
s.advertise(rcp.Capability.Retrieve, { maxK: 100, modes: ["hybrid"] });

s.on(rcp.Method.RETRIEVE, async (params) => {
  const hits = await myIndex.search(params.query, params.k ?? 10);
  return { hits: hits.map((h) => ({ id: h.id, score: h.score, text: h.text })) };
});

await s.serveStdio();

// ── client ──────────────────────────────────────────────
const c = await rcp.connectStdio(["node", "my_engine.js"]);
if (c.supports(rcp.Capability.Retrieve))
  for (const h of await c.retrieve("eiffel tower", 3))
    console.log(h.id, h.score);
```

### C++

```cpp
#include "rcp.hpp"
using namespace rcp;

struct Engine {
  PeerInfo info() const { return {"my-engine", "1.0"}; }
  Capabilities capabilities() const {
    return Capabilities{}.with_retrieve({{"maxK", 100}});
  }
  Result<Json> retrieve(const Json& p) {
    return Json{{"hits", search(p["query"], p["k"])}};
  }
};

int main() { Server{Engine{}}.serve_stdio(); }
```

### Rust

```rust
use rcp::{obj, Capability, Method, Server};

fn main() {
    let mut s = Server::new();
    s.set_info("my-engine", "1.0");
    s.advertise(Capability::Retrieve, obj(&[("maxK", 100.into())]));

    s.on(Method::RETRIEVE, |params| {
        let q = params.get_str("query").unwrap_or("");
        Ok(obj(&[("hits", search(q))]))
    });

    s.serve_stdio(); // client: rcp::connect_stdio(&["my_engine"])?.retrieve("q", 3)?
}
```

## Many engines, two models

Both routing models layer on the core `retrieve` + capability handshake — **zero
new server obligations** — and both can be driven from a single `rcp.json`
registry ([§16.1](spec/rcp-1.0.md#161-discovery)).

| Model | Use it when | API |
|-------|-------------|-----|
| **Selector** | you have several backends and want to pick **one** — by id, by required capability, or by priority with liveness fallback (lazy connect) | `rcp.Selector.load("rcp.json").select_primary()` |
| **Federation** | you want to query **all** reachable engines concurrently and fuse the ranked lists with Reciprocal Rank Fusion (origin-tagged hits) | `Federation::from_registry_file("rcp.json")` |

```json
{ "engines": [
    { "id": "docs", "transport": "stdio", "command": ["python3", "docs_server.py"], "priority": 10 },
    { "id": "web",  "transport": "http",  "url": "http://127.0.0.1:8000/rcp", "weight": 1.5 } ] }
```

## Methods

| Method | Capability | Purpose |
|--------|-----------|---------|
| `initialize` / `info` | always | version + capability handshake / stateless identity |
| `embed` | `embed` | dense embeddings |
| `embed/sparse` | `sparseEmbed` | learned-sparse terms (SPLADE) |
| `embed/multi` | `multiVector` | per-token vectors (ColBERT) |
| `rerank` | `rerank` | cross-encoder / late-interaction scoring |
| `retrieve` | `retrieve` | the workhorse — mode, filter, rerank, MMR, recency, citations |
| `query/transform` | `transform` | expansion / HyDE / decomposition |
| `graph` | `graph` | GraphRAG local / global / drift |
| `index/add`, `index/delete` | `index` | mutate the corpus |
| `catalog/list` | `catalog` | enumerate federated engines |
| `notifications/cancel` | always | abandon an in-flight request |
| `ping` | always | liveness / round-trip; echoes a nonce |
| `shutdown` | always | graceful close |

Full details, error codes, streaming, pagination, and conformance rules are in
the [**specification**](spec/rcp-1.0.md).

## Build & test

**C++** (needs a C++23 compiler with `std::expected` — GCC 13+ / recent Clang):

```sh
cd sdk/cpp
make test        # static_assert proofs + runtime checks
make examples    # example_server / client / selector / federation
```

**Python** (pure standard library — no compiler, no dependencies, no build step):

```sh
cd sdk/python
python3 test_bindings.py   # in-proc server + client↔C++ server + selector
```

**Node.js** (standard library only — no dependencies, no build step):

```sh
cd sdk/node
node test.js   # in-proc server + client↔C++ server + selector
```

**Rust** (zero dependencies — vendors its own JSON, no `serde`):

```sh
cd sdk/rust
cargo test     # in-proc server + client↔C++ server + registry selector
```

**Conformance** — validate any server, in any language:

```sh
python3 conformance/check.py -- python3 examples/example_server.py
python3 conformance/check.py -- ./sdk/cpp/example_server
```

## Repository layout

- `spec/rcp-1.0.md` — the normative specification (RFC-2119, JSON-RPC 2.0).
- `schema/rcp-1.0.json` — JSON Schema (draft 2020-12) for every message shape.
- `sdk/cpp/` — the type-theoretic C++ SDK (header-only) + examples + tests.
- `sdk/python/` — the native Python `rcp` package (standard library only).
- `sdk/node/` — the native Node.js `rcp-protocol` package (standard library only).
- `sdk/rust/` — the native Rust `rcp-protocol` crate (zero dependencies).
- `conformance/` — transport-agnostic conformance suite.
- `examples/` — runnable Python client/server.
- `docs/` — the documentation website ([Mintlify](https://mintlify.com)); each
  protocol concept is its own readable page. The spec and schema stay the source
  of truth; the site links them and syncs the schema.
- `rcp.json` — sample engine registry.

## Contributing

RCP is meant to be a community standard, not a single vendor's API. See
[`CONTRIBUTING.md`](CONTRIBUTING.md) for how to file issues, add an engine or
SDK, and run the conformance suite; [`GOVERNANCE.md`](GOVERNANCE.md) and
[`docs/community/governance.mdx`](docs/community/governance.mdx)
for how the spec evolves (additive and capability-discovered — the wire never
breaks under you); [`MAINTAINERS.md`](MAINTAINERS.md) for who stewards the
project; [`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md) for community standards; and
[`SECURITY.md`](SECURITY.md) to report a vulnerability privately.

Working with an AI agent in this repo? [`AGENTS.md`](AGENTS.md) is the
agent-readable guide to the layout, toolchains, and how to verify a change.

## License

MIT © 2026 Ayush Bhat. Contributions welcome — RCP is meant to be a community
standard, not a single vendor's API.
