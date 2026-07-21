// examples/example_federation.cpp — query EVERY reachable engine and fuse.
// Discovery here is trivial (two copies of the example server as two engines);
// in production the engine list comes from an rcp.json registry or catalog/list.
// Build: c++ -std=c++23 -Iinclude examples/example_federation.cpp -o example_federation
#include "rcp.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace rcp;

int main(int argc, char** argv) {
    std::string server = argc > 1 ? argv[1] : "./example_server";

    // Discovery: connect to each reachable engine (from a registry in real use).
    Federation fed;
    for (auto&& [id, weight] : std::vector<std::pair<std::string, double>>{{"alpha", 1.0}, {"beta", 2.0}}) {
        auto cli = Client::connect_stdio({server});
        if (!cli) { std::printf("engine %s failed to connect\n", id.c_str()); continue; }
        fed.add(id, std::move(*cli), weight);
    }
    std::printf("federation of %zu engines; any retrieve? %d ; all retrieve? %d\n",
                fed.size(), fed.any_supports(Capability::Retrieve), fed.all_support(Capability::Retrieve));

    auto k = TopK::make(3);
    auto hits = fed.retrieve("landmark in the French capital", *k);
    if (!hits) { std::printf("federated retrieve failed: %s\n", hits.error().message.c_str()); return 1; }

    std::printf("fused hits (RRF across all engines):\n");
    for (auto& h : *hits) {
        std::string engine = h.meta.contains("engine") ? h.meta["engine"].get<std::string>() : "?";
        std::printf("   %-4s rrf=%.4f  [via %s]  %s\n",
                    h.id.c_str(), h.score.get(), engine.c_str(), h.text.c_str());
    }
    return 0;
}
