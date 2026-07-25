# rcp — Retrieval Context Protocol, C++ SDK

A **header-only, type-theoretic** C++23 SDK for
[RCP](https://rcp-6d6ef6d5.mintlify.site/) — the open, versioned JSON-RPC
protocol that lets any RAG engine expose
`embed` / `rerank` / `retrieve` / `graph` / `index` / `catalog`, and any client
consume it uniformly.

The only vendored file is a single-header JSON parser (`include/json.hpp`); the
SDK itself is standard-library only and needs a C++23 toolchain with
`std::expected` (GCC 13+ / recent Clang — AppleClang does not yet qualify). It
speaks the exact same wire format as the [Python](../python), [Node.js](../node),
and [Rust](../rust) SDKs, so a C++ client and a server in any other language
interoperate byte-for-byte.

## Use it

### CMake (recommended)

Install the package and link the interface target:

```sh
cmake -S sdk/cpp -B build -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --install build
```

Then in your project:

```cmake
find_package(rcp CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE rcp::rcp)   # pulls in the headers + C++23
```

### vcpkg

The `sdk/cpp/vcpkg.json` manifest declares the port; add `rcp` to your
manifest's `dependencies` once the port is registered.

### Just the headers

It is header-only, so you can also skip CMake entirely:

```sh
g++ -std=c++23 -Isdk/cpp/include my_app.cpp -o my_app
```

## Client

The client is refinement-typed: `retrieve` takes a `TopK` that cannot be zero,
is gated on the server's advertised capabilities, and returns a
`Result<T>` (`std::expected<T, Error>`) so errors are values, not exceptions.

```cpp
#include <rcp.hpp>
using namespace rcp;

int main() {
  auto cli = Client::connect_stdio({"./example_server"});
  if (!cli) return 1;

  if (cli->supports(Capability::Retrieve)) {
    auto k = TopK::make(3);                      // TopK::make(0) would fail
    auto hits = cli->retrieve("eiffel tower", *k);
    if (hits)
      for (auto& h : *hits)
        std::printf("%s  %.3f  %s\n", h.id.c_str(), h.score.get(), h.text.c_str());
  }

  cli->shutdown();
}
```

A call to an unadvertised capability fails fast, client-side, before any I/O:
its `Result` holds an `Error` with `code == errc::CapabilityMissing` (-32003).

### Agentic & frontier RAG

The full 2024–2026 RAG surface is typed. `retrieve`/`search` opts carry `unit` /
`level` (granularity), `tokenBudget` (long-context packing), and `sessionId`
(agentic trajectories); each `Hit` surfaces `confidence` (`std::optional<double>`,
normalised [0,1]), `unit`, `level`, `scores`, `provenance`, and `trust`.
`cli->feedback(signals)` sends RL / corrective / integrity signals back
(spec §7.16), and `cli->memory_build(...)` / `cli->memory_recall(query)` drive
MemoRAG / HippoRAG memory → clues (spec §7.17). A server opts in by advertising
`with_session()` / `with_feedback()` / `with_memory()` and implementing the
`feedback` / `memory_build` / `memory_recall` hooks — each detected at compile
time by the `Handler` concept.

## Server

A server is a **handler struct** that models the `Handler` concept: `info()`,
`capabilities()`, and whichever method hooks it implements. Advertising a
capability whose hook you didn't write yields `CapabilityMissing` at dispatch —
no null-pointer surprises.

```cpp
#include <rcp.hpp>
using namespace rcp;

struct MyEngine {
  PeerInfo info() const { return {"my-engine", "1.0"}; }

  Capabilities capabilities() const {
    Capabilities c;
    c.with_retrieve(100, {"dense", "sparse", "hybrid"});
    return c;
  }

  Result<Json> retrieve(const Json& p) {
    // ... build hits from p["query"], p["k"] ...
    return Json{{"hits", hits}};
  }
};

int main() {
  Server srv{MyEngine{}};
  srv.serve_stdio();          // or srv.serve_http(8000);
}
```

## Build & test

```sh
cd sdk/cpp
make test        # static_assert proofs + runtime client ↔ server check
make all         # + selector / federation examples
```

Or via CMake with the examples enabled:

```sh
cmake -S sdk/cpp -B build -DRCP_BUILD_EXAMPLES=ON && cmake --build build
```

## Federation fusion & the vector codec

`rcp/fusion.hpp` and `rcp/vectors.hpp` are standalone, header-only, and tested —
byte-for-byte identical to the Python / Node / Rust SDKs. The live `Federation`
delegates to the same `rcp::fusion::rrf_fuse`, so held-list fusion and
fan-out fusion agree exactly:

```cpp
using rcp::fusion::EngineList;
std::vector<EngineList> engines = {{"A", a_hits, 1.0}, {"B", b_hits, 0.7}};
auto fused = rcp::fusion::rrf_fuse(engines, /*k=*/10);

auto blob = rcp::vectors::encode_f32_base64({1.5f, -2.25f, 0.0f});
auto back = rcp::vectors::decode_f32_base64(blob, /*dimension=*/3);
```

## License

MIT © 2026 Ayush Bhat.
