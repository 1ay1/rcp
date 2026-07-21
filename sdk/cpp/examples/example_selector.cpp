// examples/example_selector.cpp — pick ONE backend from several RAG engines.
//
// You have two RAG implementations reachable. The client chooses one as its
// backend by a policy, connects to just that one, and uses it normally.
// Build: c++ -std=c++23 -Iinclude examples/example_selector.cpp -o example_selector
#include "rcp.hpp"

#include <cstdio>
#include <string>

using namespace rcp;

int main(int argc, char** argv) {
    std::string server = argc > 1 ? argv[1] : "./example_server";

    // Register the reachable backends (from an rcp.json registry in real use).
    // `primary` is preferred (higher priority); `backup` is the fallback.
    Selector sel;
    sel.add(EngineSpec::stdio("primary", {server}, /*priority=*/10))
       .add(EngineSpec::stdio("backup",  {server}, /*priority=*/1));

    // Policy 1: pick a specific backend by name.
    if (auto c = sel.select("backup")) {
        std::printf("[by-name]      chose '%s' -> %s\n", "backup", c->server().name.c_str());
    }

    // Policy 2: primary → secondary with liveness fallback (use the preferred
    // engine that actually connects). This is the "two backends, pick one" case.
    auto chosen = sel.select_primary();
    if (!chosen) { std::printf("no backend available: %s\n", chosen.error().message.c_str()); return 1; }
    std::printf("[primary]      chose highest-priority live backend -> %s\n", chosen->server().name.c_str());

    // Policy 3: first backend that can do what we need (e.g. graph search).
    if (auto c = sel.select_capable(Capability::Graph)) {
        std::printf("[by-capability] first graph-capable backend -> %s\n", c->server().name.c_str());

        // From here it is an ordinary single-backend client.
        if (auto k = TopK::make(2); k) {
            auto hits = c->retrieve("landmark in the French capital", *k);
            if (hits) for (auto& h : *hits)
                std::printf("     %-4s score=%.3f  %s\n", h.id.c_str(), h.score.get(), h.text.c_str());
        }
    } else {
        std::printf("[by-capability] no graph-capable backend: %s\n", c.error().message.c_str());
    }

    // Policy 4: build the whole Selector from an rcp.json registry (§16.1).
    if (argc > 2) {
        auto reg = Selector::from_registry_file(argv[2]);
        if (reg) {
            std::printf("[registry]     loaded %zu engine(s) from %s\n", reg->size(), argv[2]);
            if (auto c = reg->select_primary())
                std::printf("[registry]     primary -> %s\n", c->server().name.c_str());
        } else {
            std::printf("[registry]     load failed: %s\n", reg.error().message.c_str());
        }
    }
    return 0;
}
