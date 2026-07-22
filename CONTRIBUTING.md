# Contributing to RCP

RCP is meant to be a **community standard, not a single vendor's API.** The wire
is small, the spec is public, and every reference SDK is dependency-free.
Contributions of every size are welcome.

The full contributor guide lives in the docs:
**https://github.com/1ay1/rcp/blob/main/docs/community/contributing.mdx**
(rendered at the docs site under **Community → Contributing**).

## Ways to contribute

- **File an issue** — report a spec ambiguity, an SDK bug, or a docs gap.
  Ambiguities in a protocol spec are bugs; please report them.
- **Implement an engine** — wrap your retrieval stack behind an RCP server and
  it works with every RCP client. Prove it with the conformance suite.
- **Write an SDK** — a new language binding is the highest-leverage
  contribution. Mirror the existing SDKs: same wire, same method names, same
  errors.
- **Sharpen the spec or docs** — `spec/rcp-1.0.md` is the source of truth; the
  docs site is authored in MDX under `docs/`.

## Repository layout

| Path | What lives there |
|------|------------------|
| `spec/rcp-1.0.md` | The normative RCP/1 specification. |
| `schema/rcp-1.0.json` | JSON Schema for every message shape. |
| `sdk/{cpp,python,node,rust}` | The four reference SDKs. |
| `conformance/check.py` | The transport-agnostic conformance runner. |
| `examples/` | Minimal example servers and clients. |
| `docs/` | The Mintlify documentation site. |

## Local setup

Each SDK is self-contained and dependency-free. Build and test the one you touch:

```sh
cd sdk/rust && cargo test && cargo clippy      # Rust
cd sdk/node && node test.js                     # Node.js
python3 sdk/python/test_bindings.py             # Python
cd sdk/cpp && make example_server example_client # C++ (needs a C++23 compiler)
```

## The bar for a change

Every change must keep the four SDKs **byte-for-byte interoperable**:

1. Run the conformance suite against a reference engine — it must pass **10/10**:
   `python3 conformance/check.py -- ./sdk/rust/target/debug/examples/example_server`
2. If you touched the wire, confirm a client in one language still drives a
   server in another (the SDK test suites include cross-language cases).
3. Update `spec/rcp-1.0.md` **and** `schema/rcp-1.0.json` together with any wire
   change, and regenerate the docs.
4. Keep it **additive** — within RCP/1, new behaviour must be
   capability-discovered, never a breaking change. See
   [`docs/community/governance.mdx`](docs/community/governance.mdx).

Have an idea that changes the wire? Open a discussion issue **before** writing
the code — and check whether `_meta` / `x-` extensions already cover your need
without a spec change.

## Code of conduct

Participation is governed by our [Code of Conduct](CODE_OF_CONDUCT.md).
