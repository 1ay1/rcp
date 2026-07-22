---
title: SDKs
description: The type-theoretic C++23 SDK and its Python bindings.
sidebar:
  order: 1
---

RCP ships **one type-theoretic C++ SDK** (header-only) with **Python bindings**
layered over it via pybind11. Cross-language interop is proven by the test suite:
a Python client drives a C++ server and vice versa.

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

## Python — pybind11 bindings

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
python -m venv .venv && . .venv/bin/activate
pip install pybind11 setuptools
python setup.py build_ext --inplace
python test_bindings.py
```

SDKs pass capability metadata as opaque JSON via `with_*({...})`, so new fields
like modality, similarity, and determinism flags work without new plumbing.
