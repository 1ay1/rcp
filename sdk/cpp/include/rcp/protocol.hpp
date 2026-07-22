#pragma once
// rcp/protocol.hpp — the RCP wire vocabulary as typed values.
//
// Method names and error codes are compile-time constants. Capabilities model
// `presence ⇒ supported` as std::optional<Json> (absent = nullopt, not an empty
// object), so a capability check is a total match on a typed enum rather than
// poking strings in a blob. This is what lets the client (client.hpp) gate calls
// on capabilities without ad-hoc string comparison.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rcp/types.hpp"

namespace rcp {

namespace method {
inline constexpr std::string_view Initialize  = "initialize";
inline constexpr std::string_view Info        = "info";
inline constexpr std::string_view Embed       = "embed";
inline constexpr std::string_view EmbedSparse = "embed/sparse";
inline constexpr std::string_view EmbedMulti  = "embed/multi";
inline constexpr std::string_view Rerank      = "rerank";
inline constexpr std::string_view Retrieve    = "retrieve";
inline constexpr std::string_view Transform   = "query/transform";
inline constexpr std::string_view Graph       = "graph";
inline constexpr std::string_view IndexAdd    = "index/add";
inline constexpr std::string_view IndexDelete = "index/delete";
inline constexpr std::string_view Catalog     = "catalog/list";
inline constexpr std::string_view Cancel      = "notifications/cancel";
inline constexpr std::string_view Log         = "log";
inline constexpr std::string_view Ping        = "ping";
inline constexpr std::string_view Shutdown    = "shutdown";
} // namespace method

// ── PeerInfo ─────────────────────────────────────────────────────────────────
struct PeerInfo {
    std::string name    = "unknown";
    std::string version = "0";
    [[nodiscard]] Json to_json() const { return Json{{"name", name}, {"version", version}}; }
    static PeerInfo from_json(const Json& j) {
        PeerInfo p;
        if (j.is_object()) { p.name = j.value("name", p.name); p.version = j.value("version", p.version); }
        return p;
    }
};

// ── Capability lattice ───────────────────────────────────────────────────────
// A typed enum so capability checks are exhaustive matches, not string compares.
enum class Capability {
    Embed, SparseEmbed, MultiVector, Rerank, Retrieve, Transform, Graph, Index
};

[[nodiscard]] constexpr std::string_view to_string(Capability c) noexcept {
    switch (c) {
        case Capability::Embed:       return "embed";
        case Capability::SparseEmbed: return "sparseEmbed";
        case Capability::MultiVector: return "multiVector";
        case Capability::Rerank:      return "rerank";
        case Capability::Retrieve:    return "retrieve";
        case Capability::Transform:   return "transform";
        case Capability::Graph:       return "graph";
        case Capability::Index:       return "index";
    }
    return "?";
}

// presence ⇒ supported. Each optional is nullopt (absent) or a metadata object.
struct Capabilities {
    std::optional<Json> embed, sparse_embed, multi_vector, rerank, retrieve, transform, graph, index;
    bool streaming = false, pagination = false, citations = false, log_ = false;

    [[nodiscard]] const std::optional<Json>& slot(Capability c) const noexcept {
        switch (c) {
            case Capability::Embed:       return embed;
            case Capability::SparseEmbed: return sparse_embed;
            case Capability::MultiVector: return multi_vector;
            case Capability::Rerank:      return rerank;
            case Capability::Retrieve:    return retrieve;
            case Capability::Transform:   return transform;
            case Capability::Graph:       return graph;
            case Capability::Index:       return index;
        }
        return embed; // unreachable
    }
    [[nodiscard]] bool has(Capability c) const noexcept { return slot(c).has_value(); }

    // Ergonomic builders (fluent) for a server advertising what it offers.
    Capabilities& with_embed(Dimension dim, std::string identity,
                             std::vector<std::string> modalities = {}) {
        embed = Json{{"dimension", dim.get()}, {"identity", std::move(identity)}, {"normalized", true}};
        if (!modalities.empty()) (*embed)["modalities"] = std::move(modalities);
        return *this;
    }
    Capabilities& with_sparse(std::string identity, std::size_t vocabulary = 0) {
        sparse_embed = Json{{"identity", std::move(identity)}};
        if (vocabulary) (*sparse_embed)["vocabulary"] = vocabulary;
        return *this;
    }
    Capabilities& with_multi(Dimension dim, std::string identity,
                             std::string similarity = "dot",
                             std::vector<std::string> modalities = {}) {
        multi_vector = Json{{"dimension", dim.get()}, {"identity", std::move(identity)},
                            {"similarity", std::move(similarity)}};
        if (!modalities.empty()) (*multi_vector)["modalities"] = std::move(modalities);
        return *this;
    }
    Capabilities& with_retrieve(std::size_t max_k, std::vector<std::string> modes,
                                std::vector<std::string> modalities = {}) {
        retrieve = Json{{"maxK", max_k}, {"modes", std::move(modes)}, {"fusion", {"rrf"}}, {"mmr", true}};
        if (!modalities.empty()) (*retrieve)["modalities"] = std::move(modalities);
        return *this;
    }
    Capabilities& with_rerank(std::vector<std::string> methods) {
        rerank = Json{{"methods", std::move(methods)}};
        return *this;
    }
    Capabilities& with_graph(std::vector<std::string> ops) {
        graph = Json{{"ops", std::move(ops)}};
        return *this;
    }
    Capabilities& with_transform(std::vector<std::string> methods) {
        transform = Json{{"methods", std::move(methods)}};
        return *this;
    }
    // Free-form opt-ins for object-valued flags and vendor/extension keys.
    Capabilities& with_streaming()  { streaming = true;  return *this; }
    Capabilities& with_pagination() { pagination = true; return *this; }
    Capabilities& with_citations()  { citations = true;  return *this; }
    Capabilities& with_log()        { log_ = true;       return *this; }

    [[nodiscard]] Json to_json() const {
        Json j = Json::object();
        auto put = [&](const char* k, const std::optional<Json>& v) { if (v) j[k] = *v; };
        put("embed", embed);           put("sparseEmbed", sparse_embed);
        put("multiVector", multi_vector); put("rerank", rerank);
        put("retrieve", retrieve);     put("transform", transform);
        put("graph", graph);           put("index", index);
        // Object-form presence flags (spec §6): presence ⇒ supported.
        if (streaming)  j["streaming"]  = Json::object();
        if (pagination) j["pagination"] = Json::object();
        if (citations)  j["citations"]  = Json::object();
        if (log_)       j["log"]        = Json::object();
        return j;
    }
    static Capabilities from_json(const Json& j) {
        Capabilities c;
        if (!j.is_object()) return c;
        auto get = [&](const char* k) -> std::optional<Json> {
            auto it = j.find(k);
            if (it != j.end() && !it->is_null()) return *it;
            return std::nullopt;
        };
        c.embed = get("embed"); c.sparse_embed = get("sparseEmbed"); c.multi_vector = get("multiVector");
        c.rerank = get("rerank"); c.retrieve = get("retrieve"); c.transform = get("transform");
        c.graph = get("graph"); c.index = get("index");
        // A flag is "present" whether it arrived as an object (§6) or a legacy bool.
        c.streaming  = j.contains("streaming")  && !j["streaming"].is_null();
        c.pagination = j.contains("pagination") && !j["pagination"].is_null();
        c.citations  = j.contains("citations")  && !j["citations"].is_null();
        c.log_       = j.contains("log")        && !j["log"].is_null();
        return c;
    }
};

// ── initialize handshake ─────────────────────────────────────────────────────
struct InitializeResult {
    int          protocol_version = kProtocolVersion;
    PeerInfo     server;
    Capabilities capabilities;

    [[nodiscard]] Json to_json() const {
        return Json{{"protocolVersion", protocol_version},
                    {"server", server.to_json()},
                    {"capabilities", capabilities.to_json()}};
    }
    static InitializeResult from_json(const Json& j) {
        InitializeResult r;
        r.protocol_version = j.value("protocolVersion", kProtocolVersion);
        if (j.contains("server"))       r.server       = PeerInfo::from_json(j["server"]);
        if (j.contains("capabilities")) r.capabilities = Capabilities::from_json(j["capabilities"]);
        return r;
    }
};

// ── typed hit (spec §7.7) ────────────────────────────────────────────────────
struct Hit {
    std::string id;
    Score       score{0.0};
    std::string text;
    std::string modality;   // "text"|"image"|"audio"|"code"|"multimodal" (§4.8)
    Json        meta;
    Json        citation;
    Json        trust;      // provenance {level, score?} (§15.2)
    Json        content;    // non-text bodies [Content] (§4.8)

    static Hit from_json(const Json& j) {
        Hit h;
        if (j.contains("id")) h.id = j["id"].is_string() ? j["id"].get<std::string>() : j["id"].dump();
        h.score = Score{j.value("score", 0.0)};
        h.text  = j.value("text", std::string{});
        h.modality = j.value("modality", std::string{});
        if (j.contains("meta"))     h.meta = j["meta"];
        if (j.contains("citation")) h.citation = j["citation"];
        if (j.contains("trust"))    h.trust = j["trust"];
        if (j.contains("content"))  h.content = j["content"];
        return h;
    }
};

static_assert(std::is_default_constructible_v<Capabilities>,
              "a server starts from an empty capability set and opts in");

} // namespace rcp
