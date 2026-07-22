// examples/example_server.cpp — a type-theoretic RCP server.
// Build: c++ -std=c++23 -Iinclude examples/example_server.cpp -o example_server
// Run:   ./example_server            (stdio)   |   ./example_server --http 8000
#include "rcp.hpp"

#include <cmath>
#include <string>
#include <vector>

using namespace rcp;

namespace {
constexpr std::size_t DIM = 384;

std::vector<float> embed_one(const std::string& text) {
    std::vector<float> v(DIM, 0.0f);
    std::string tok;
    auto flush = [&] {
        if (tok.empty()) return;
        std::uint64_t h = 1469598103934665603ull;
        for (char c : tok) { h ^= (unsigned char)std::tolower(c); h *= 1099511628211ull; }
        v[h % DIM] += 1.0f; tok.clear();
    };
    for (char c : text) { if (std::isspace((unsigned char)c)) flush(); else tok.push_back(c); }
    flush();
    float n = 0; for (float x : v) n += x * x; n = n > 0 ? 1.0f / std::sqrt(n) : 0;
    for (float& x : v) x *= n;
    return v;
}
float cosine(const std::vector<float>& a, const std::vector<float>& b) {
    float s = 0; for (std::size_t i = 0; i < a.size(); ++i) s += a[i] * b[i]; return s;
}
const std::vector<std::pair<std::string, std::string>> DOCS = {
    {"d1", "The Eiffel Tower is a wrought-iron lattice tower in Paris, France."},
    {"d2", "Photosynthesis converts light energy into chemical energy in plants."},
    {"d3", "The Great Wall of China stretches thousands of kilometres."},
};
} // namespace

// A handler models the Handler concept: info + capabilities + whichever method
// hooks it implements. Advertising a capability whose hook you didn't write
// would just yield CapabilityMissing at dispatch — no null-pointer surprises.
struct ExampleEngine {
    PeerInfo info() const { return {"rcp-cpp-example", "1.0.0"}; }

    Capabilities capabilities() const {
        Capabilities c;
        c.with_embed(Dimension{DIM}, "toy-hash-v1")
         .with_retrieve(100, {"dense", "sparse", "hybrid"})
         .with_graph({"local", "global"})
         .with_session()
         .with_feedback()
         .with_memory({"global"}, true);
        c.citations = true;
        return c;
    }

    Result<Json> embed(const Json& p) {
        // Accept `inputs` (preferred) or the legacy `texts`; each element is a
        // bare string or a Content block (spec §4.8 / §7.3).
        const Json& in = p.contains("inputs") ? p["inputs"] : p.value("texts", Json::array());
        Json vecs = Json::array();
        for (auto& t : in) {
            std::string s = t.is_string() ? t.get<std::string>() : t.value("text", std::string{});
            vecs.push_back(embed_one(s));
        }
        return Json{{"vectors", std::move(vecs)}};
    }

    Result<Json> retrieve(const Json& p) {
        std::string q = p.value("query", std::string{});
        std::size_t k = p.value("k", std::size_t{10});
        auto qv = embed_one(q);
        std::vector<std::pair<float, std::size_t>> scored;
        for (std::size_t i = 0; i < DOCS.size(); ++i) scored.push_back({cosine(qv, embed_one(DOCS[i].second)), i});
        std::sort(scored.begin(), scored.end(), [](auto& a, auto& b) { return a.first > b.first; });
        Json hits = Json::array();
        for (std::size_t r = 0; r < scored.size() && r < k; ++r) {
            auto& [s, i] = scored[r];
            hits.push_back(Json{{"id", DOCS[i].first}, {"score", s}, {"text", DOCS[i].second},
                                {"confidence", s < 0 ? 0.0 : (s > 1 ? 1.0 : s)},  // normalised [0,1]
                                {"unit", p.value("unit", std::string{"chunk"})},
                                {"citation", {{"source", DOCS[i].first}}},
                                {"trust", {{"level", "trusted"}}}});
        }
        return Json{{"hits", std::move(hits)},
                    {"usage", {{"mode", p.value("mode", std::string{"hybrid"})},
                               {"candidates", DOCS.size()},
                               {"reranked", 0}}}};
    }

    Result<Json> graph(const Json& p) {
        if (p.value("op", std::string{}) == "global")
            return Json{{"summary", "a global community summary"}, {"communities", Json::array()}};
        return retrieve(p);
    }

    // Client→server signals (spec §7.16): a real engine would log or online-update;
    // the example just counts what it accepted.
    Result<Json> feedback(const Json& p) {
        std::size_t n = p.contains("signals") && p["signals"].is_array() ? p["signals"].size() : 0;
        return Json{{"accepted", n}};
    }

    // Global/session memory (spec §7.17): a real engine would summarise the corpus;
    // the example returns a stub handle and a couple of clues.
    Result<Json> memory_build(const Json&) {
        return Json{{"memoryId", "mem-example"}, {"tokens", 0}};
    }
    Result<Json> memory_recall(const Json& p) {
        return Json{{"clues", Json::array({
            Json{{"query", p.value("query", std::string{})}, {"weight", 1.0}},
            Json{{"seedIds", Json::array({"d1"})}, {"weight", 0.5}}})}};
    }
};

int main(int argc, char** argv) {
    Server srv{ExampleEngine{}};
    if (argc >= 3 && std::string(argv[1]) == "--http") {
        auto r = srv.serve_http(static_cast<std::uint16_t>(std::atoi(argv[2])));
        return r ? 0 : 1;
    }
    srv.serve_stdio();
    return 0;
}
