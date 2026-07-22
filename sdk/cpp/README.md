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

## License

MIT © 2026 Ayush Bhat.
