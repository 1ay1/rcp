# rcp — Retrieval Context Protocol, Rust SDK

A **native, zero-dependency** Rust SDK for [RCP](https://github.com/1ay1/rcp) —
the open, versioned JSON-RPC protocol that lets any RAG engine expose `embed`,
`rerank`, `retrieve`, `graph`, `index`, and `catalog`, and any client consume it
uniformly.

Built entirely on the Rust standard library — **no dependencies** (like the C++
SDK vendors `json.hpp`, this crate vendors a minimal JSON value type). It speaks
the exact same wire format as the [C++](../cpp), [Python](../python), and
[Node.js](../node) SDKs, so a Rust client and a C++/Python/Node server (or
vice-versa) interoperate byte-for-byte. Synchronous and blocking; every call
returns `Result<_, RcpError>`.

## Add it

```toml
[dependencies]
rcp-protocol = { path = "sdk/rust" }   # or a git/version dependency
```

The crate is imported as `rcp`. Requires Rust 1.70+.

## Client

Connect over a subprocess (stdio) or HTTP, then make typed, capability-gated
calls. A call to a capability the server never advertised returns
`Err(CapabilityMissing)` **before** any round-trip.

```rust
use rcp::Capability;

fn main() -> Result<(), rcp::RcpError> {
    let mut c = rcp::connect_stdio(&["python3", "my_server.py"])?;
    // or: let mut c = rcp::connect_http("http://127.0.0.1:8000/rcp")?;

    println!("{} (RCP/{})", c.server(), c.protocol_version());

    if c.supports(Capability::Retrieve) {
        for hit in c.retrieve("eiffel tower", 3)? {
            println!("{} {} {}", hit.id, hit.score, hit.text);
        }
    }

    if c.supports(Capability::Embed) {
        let vecs = c.embed(&["hello world"], None)?;
        println!("dim {}", vecs[0].len());
    }

    c.shutdown();
    Ok(())
}
```

The error carries the wire code and message (`Display` renders `"[RCP <code>]
<message>"`):

```rust
match c.rerank("q", &["a", "b"]) {
    Err(e) => assert_eq!(e.code, rcp::Errc::CAPABILITY_MISSING), // -32003
    Ok(_) => {}
}
```

Client methods: `embed`, `embed_sparse`, `embed_multi`, `rerank`, `retrieve`,
`search` (returns `SearchResult { hits, usage, next_cursor }`), `graph`,
`transform`, `index_add`, `index_delete`, `catalog`, `info`, `ping`, `call`,
`shutdown`.

## Server

Handlers are `FnMut(&Json) -> Result<Json, RcpError>`.

```rust
use rcp::{obj, Capability, Json, Method, Server};

fn main() {
    let mut s = Server::new();
    s.set_info("my-engine", "1.0");
    s.advertise(Capability::Retrieve, obj(&[("maxK", 100.into())]));

    s.on(Method::RETRIEVE, |params| {
        let q = params.get_str("query").unwrap_or("");
        Ok(obj(&[("hits", search(q))]))
    });

    s.serve_stdio();          // or: s.serve_http(8000).unwrap();
}
```

The `Server` owns the `initialize` handshake, capability gating, JSON-RPC framing
and batching, and error mapping. It answers `initialize` / `info` / `ping` /
`shutdown` itself; a gated call before `initialize` is `-32001`, an unadvertised
or unimplemented method is `-32003`, an unknown method is `-32004`.

## Selecting a backend

```rust
let sel = rcp::Selector::load("rcp.json")?;
let mut c = sel.select_capable(rcp::Capability::Retrieve)?;
```

## Examples & tests

```sh
cargo run --example example_server            # stdio (default)
cargo run --example example_server -- --http 8000
cargo run --example example_client            # drives the example server

cargo test                                    # incl. Rust client <-> C++ server
```

## The JSON value type

`rcp::Json` is a small, ordered JSON value with `parse`, `Display`/`dump`,
`get`/`get_str`/`as_*` accessors, `insert`/`push`, and `From` impls, plus the
`rcp::obj(&[(key, value)])` builder. It exists so the SDK stays dependency-free;
you never need `serde` to speak RCP.

## License

MIT © 2026 Ayush Bhat.
