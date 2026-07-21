// examples/example_client.cpp — a type-theoretic RCP client.
// The client only exists in the Ready state; retrieve() takes a TopK that
// cannot be zero and is gated on the server's advertised capabilities.
#include "rcp.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace rcp;

int main(int argc, char** argv) {
    std::vector<std::string> server = {"./example_server"};
    if (argc > 1) server = {argv[1]};

    auto cli = Client::connect_stdio(server);
    if (!cli) { std::printf("connect failed: %s\n", cli->server().name.c_str()); return 1; }

    std::printf("connected to %s v%s (RCP/%d)\n",
                cli->server().name.c_str(), cli->server().version.c_str(), cli->protocol_version());
    std::printf("supports: retrieve=%d graph=%d rerank=%d embed=%d\n",
                cli->supports(Capability::Retrieve), cli->supports(Capability::Graph),
                cli->supports(Capability::Rerank), cli->supports(Capability::Embed));

    // TopK is a refinement type: TopK::make(0) would fail; here we know 2 is fine.
    auto k = TopK::make(2);
    if (!k) { std::printf("bad k\n"); return 1; }

    auto hits = cli->retrieve("landmark in the French capital", *k);
    if (hits) {
        std::printf("retrieve -> %zu hits:\n", hits->size());
        for (auto& h : *hits)
            std::printf("   %-4s score=%.3f  %s\n", h.id.c_str(), h.score.get(), h.text.c_str());
    } else {
        std::printf("retrieve error [%d]: %s\n", hits.error().code, hits.error().message.c_str());
    }

    // Calling an unadvertised capability fails fast, client-side, before any I/O.
    std::vector<std::string> passages = {"a", "b"};
    auto rr = cli->rerank("q", passages);
    std::printf("rerank (unadvertised) -> %s\n",
                rr ? "ok" : ("gated: " + rr.error().message).c_str());

    auto g = cli->graph("global", Json{{"query", "x"}});
    if (g) std::printf("graph(global) -> %s\n", g->dump().c_str());

    return 0;
}
