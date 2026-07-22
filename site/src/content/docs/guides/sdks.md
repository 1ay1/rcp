---
title: SDKs
description: The type-theoretic C++23 SDK and the native Python and Node.js SDKs.
sidebar:
  order: 1
---

RCP ships **three SDKs** that speak the identical wire format: a **type-theoretic
C++23 SDK** (header-only), a **native Python SDK**, and a **native Node.js SDK**.
The Python and Node SDKs are pure standard library — no compiler, no
dependencies, nothing to build. Cross-language interop is proven by the test
suites: every client drives every server, in any combination.

## C++ — invariants in the type system

The C++ SDK pushes protocol invariants into the type system, proved at
**compile time**:

- **Strong scalars & refinement types** — `TopK` cannot be `0`,
  `ProtocolVersion` is validated at construction, `Score`/`Dimension` are not
  interchangeable.
- **Typestate `Client`** — only constructible via a `connect*()` that runs the
  handshake; capability-gated calls fail *client-side* with `CapabilityMissing`
  before any I/O.
- **Concept-gated `Server<H>`** — a handler advertises capabilities and
  implements matching hooks; `if constexpr` dispatch means an
  advertised-but-unimplemented method is a typed error, never a crash.
- **`Result<T> = std::expected<T, Error>`** everywhere — no exceptions for
  control flow. `test_types.cpp` carries `static_assert` proofs; **the build is
  the test runner.**

```sh
cd sdk/cpp
make test        # static_assert proofs + runtime checks
make examples    # example_server / client / selector / federation
```

Requires a C++23 compiler with `std::expected` (GCC 13+ / recent Clang).

## Python — native, standard library only

The Python SDK is written entirely against the standard library (`json`,
`subprocess`, `http.client`, `socket`) — no C++, no pybind11, no compiled
extension. It mirrors the C++ SDK's public surface and wire behaviour, so the two
interoperate byte-for-byte.

```python
import rcp

s = rcp.Server()
s.set_info("my-engine", "1.0")
s.advertise(rcp.Capability.Retrieve, {"maxK": 100, "modes": ["hybrid"]})

@s.on(rcp.Method.RETRIEVE)
def _(params):
    hits = my_index.search(params["query"], params.get("k", 10))
    return {"hits": [{"id": h.id, "score": h.score, "text": h.text} for h in hits]}

s.serve_stdio()
```

```sh
cd sdk/python
python3 test_bindings.py   # pure stdlib — no build, no dependencies
```

## Node.js — native, standard library only

The Node.js SDK is written entirely against the standard library
(`child_process`, `http`, `net`, `readline`) — no native addon, nothing to
build. It is ESM and async (every call returns a Promise), and mirrors the same
public surface and wire behaviour, so it interoperates byte-for-byte with the
C++ and Python SDKs.

```js
import * as rcp from "rcp-protocol";

const s = new rcp.Server();
s.setInfo("my-engine", "1.0");
s.advertise(rcp.Capability.Retrieve, { maxK: 100, modes: ["hybrid"] });

s.on(rcp.Method.RETRIEVE, async (params) => {
  const hits = await myIndex.search(params.query, params.k ?? 10);
  return { hits: hits.map((h) => ({ id: h.id, score: h.score, text: h.text })) };
});

await s.serveStdio();
```

```sh
cd sdk/node
node test.js   # stdlib only — no build, no dependencies
```

SDKs pass capability metadata as opaque JSON via `with_*({...})`, so new fields
like modality, similarity, and determinism flags work without new plumbing.
