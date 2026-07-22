# AGENTS.md

Guidance for AI agents (and humans) working in this repository.

## What this repo is

RCP (Retrieval Context Protocol) is an open, versioned **JSON-RPC 2.0** protocol
that standardises how a client obtains retrieval context (embed, rerank,
retrieve, graph, index, catalog) from any RAG engine — any language, any vendor.
It is the retrieval companion to MCP (tools) and ACP (agents).

The repository contains:

| Path | What it is | Source of truth? |
|------|-----------|------------------|
| `spec/rcp-1.0.md` | The normative specification (RFC-2119, §1–17 + appendices) | **Yes** |
| `schema/rcp-1.0.json` | The normative JSON Schema for the wire format | **Yes** |
| `sdk/{cpp,python,node,rust}/` | Four reference SDKs speaking the identical wire | Yes (per language) |
| `examples/` | Runnable example server + client | — |
| `conformance/check.py` | Conformance harness (L0/L1/L2) | — |
| `docs/` | Mintlify documentation site | Derived — must match spec |

**The code, spec, and schema are the source of truth — never the docs or
comments.** When they disagree, fix the docs to match the code.

## Golden rules

- Keep the four SDKs **wire-compatible**: any client must talk to any server.
- The spec and schema are normative. A docs change that contradicts them is a
  bug in the docs.
- Everything is **dependency-free and idiomatic** per language. Do not add
  third-party deps to the SDKs (Python is stdlib-only; Node is stdlib-only; the
  only vendored file is `sdk/cpp/include/json.hpp`).
- Do not write README/summary files about your work unless explicitly asked.

## Toolchains

- **Node** ≥ 20 (CI uses 20). Do not use APIs newer than Node 20 in
  `docs/scripts/*.mjs` — e.g. `fs.globSync` is not stable on 20; the vendored
  `docs/scripts/lib.mjs` `listFiles()` walker exists for exactly this reason.
- **Python** 3.13 venv at `sdk/python/.venv/`; SDK is stdlib-only.
- **Rust** 1.9x (`cargo`); crate is `rcp-protocol` (path dep, unpublished).
- **C++23**: build with `/opt/homebrew/bin/g++-15` (needs `std::expected`;
  AppleClang does not work). `cd sdk/cpp && CXX=/opt/homebrew/bin/g++-15 make example_server example_client`.

## Verifying a change

### Docs (after editing anything under `docs/`, `spec/`, or `schema/`)

```
cd docs
node scripts/sync-schema.mjs   # copies schema/rcp-1.0.json into docs/schema/
node scripts/check-config.mjs  # docs.json footguns: fonts, missing assets, redirects
node scripts/check-nav.mjs     # every docs.json page resolves to a file
node scripts/check-links.mjs   # no broken internal links
node scripts/check-mdx.mjs     # balanced components, no stray { or < in prose
```

The Mintlify CLI (`mint`) does **not** run on Node 25+, so rely on these
scripts, not local `mint dev`. The site auto-deploys from `docs/` on push to
`main` via the Mintlify GitHub App.

### SDKs / examples

```
python3 examples/example_client.py                      # against the Python server
node sdk/node/examples/example_client.js                # against the Node server
cargo run --manifest-path sdk/rust/Cargo.toml --example example_client
python3 conformance/check.py                            # conformance harness
```

Example clients import SDKs **from source** (unpublished): Node uses the relative
`./sdk/node/src/index.js`, Python needs `PYTHONPATH=sdk/python`, Rust uses a path
dependency, C++ adds `sdk/cpp/include` to the include path.

## MDX authoring rules (Mintlify)

- No literal `{` or `<` in prose — put JSON and code inside fenced code blocks.
  (`<` is allowed only when immediately followed by a letter, `/`, or `!`.)
- Block components (`Card`, `Steps`, `Tabs`, `Accordion`, `Note`, `Tip`,
  `CodeGroup`, `ParamField`, …) must balance open/close tags.
- Document parameters with `<ParamField path="…" type="…" required>` and
  responses with `<ResponseField>`, matching the existing method pages.

## Commit conventions

Commit messages are detailed and explain **what changed and why**. Group related
changes; keep the spec, schema, SDKs, and docs consistent within a single
commit where practical.
