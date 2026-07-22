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
inline constexpr std::string_view Feedback    = "feedback";
inline constexpr std::string_view MemoryBuild = "memory/build";
inline constexpr std::string_view MemoryRecall= "memory/recall";
inline constexpr std::string_view Catalog     = "catalog/list";
inline constexpr std::string_view Cancel      = "notifications/cancel";
inline constexpr std::string_view Progress    = "notifications/progress";
inline constexpr std::string_view Log         = "notifications/log";
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
    Embed, SparseEmbed, MultiVector, Rerank, Retrieve, Transform, Graph, Index,
    Session, Feedback, Memory, Catalog
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
        case Capability::Session:     return "session";
        case Capability::Feedback:    return "feedback";
        case Capability::Memory:      return "memory";
        case Capability::Catalog:     return "catalog";
    }
    return "?";
}

// presence ⇒ supported. Each optional is nullopt (absent) or a metadata object.
struct Capabilities {
    std::optional<Json> embed, sparse_embed, multi_vector, rerank, retrieve, transform, graph, index,
                        session, feedback, memory, catalog;
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
            case Capability::Session:     return session;
            case Capability::Feedback:    return feedback;
            case Capability::Memory:      return memory;
            case Capability::Catalog:     return catalog;
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
    // Advertise this server as an aggregator/gateway over `engines` downstream
    // RCP engines (spec §7.13); enables `catalog/list`.
    Capabilities& with_index(bool writable = true) {
        index = Json{{"writable", writable}};
        return *this;
    }
    // Agentic/session state for iterative retrieval (spec §7.7.3).
    Capabilities& with_session(bool dedup = true) {
        session = Json{{"dedup", dedup}};
        return *this;
    }
    // Accept client→server relevance/reward/integrity signals (spec §7.16).
    Capabilities& with_feedback() {
        feedback = Json::object();
        return *this;
    }
    // Global/session memory build + clue recall (spec §7.17).
    Capabilities& with_memory(std::vector<std::string> scopes = {"global"}, bool clues = true) {
        memory = Json{{"scopes", std::move(scopes)}, {"clues", clues}};
        return *this;
    }
    Capabilities& with_catalog(std::size_t engines = 0) {
        catalog = Json::object();
        if (engines) (*catalog)["engines"] = engines;
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
        put("session", session);       put("feedback", feedback);
        put("memory", memory);         put("catalog", catalog);
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
        c.session = get("session"); c.feedback = get("feedback"); c.memory = get("memory");
        c.catalog = get("catalog");
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
    std::string unit;       // granularity: chunk|document|node|triplet|…|page (§7.7.2)
    std::optional<double> confidence;  // normalised [0,1] self-eval (§7.7.2)
    std::optional<std::int64_t> level; // abstraction level (RAPTOR depth, Leiden) (§7.7.2)
    Json        meta;
    Json        citation;
    Json        scores;     // per-stage breakdown {dense,sparse,rerank} (§7.7)
    Json        provenance; // graph/tree lineage {path,nodes,edges,leaves} (§7.7.2)
    Json        trust;      // provenance + safety {level, score?, injectionSuspected?} (§15.2)
    Json        content;    // non-text bodies [Content] (§4.8)

    static Hit from_json(const Json& j) {
        Hit h;
        if (j.contains("id")) h.id = j["id"].is_string() ? j["id"].get<std::string>() : j["id"].dump();
        h.score = Score{j.value("score", 0.0)};
        h.text  = j.value("text", std::string{});
        h.modality = j.value("modality", std::string{});
        h.unit  = j.value("unit", std::string{});
        if (auto it = j.find("confidence"); it != j.end() && it->is_number())
            h.confidence = it->get<double>();
        if (auto it = j.find("level"); it != j.end() && it->is_number_integer())
            h.level = it->get<std::int64_t>();
        if (j.contains("meta"))       h.meta = j["meta"];
        if (j.contains("citation"))   h.citation = j["citation"];
        if (j.contains("scores"))     h.scores = j["scores"];
        if (j.contains("provenance")) h.provenance = j["provenance"];
        if (j.contains("trust"))      h.trust = j["trust"];
        if (j.contains("content"))    h.content = j["content"];
        return h;
    }
};

static_assert(std::is_default_constructible_v<Capabilities>,
              "a server starts from an empty capability set and opts in");

// ── typed retrieve result (spec §7.7 / §10) ───────────────────────────────────
// The full result envelope: the ranked hits plus the `usage` telemetry and the
// opaque pagination `nextCursor`. `Client::search` returns this; the ergonomic
// `Client::retrieve` returns just the hits.
struct RetrieveResult {
    std::vector<Hit>           hits;
    Json                       usage;        // {mode, candidates?, reranked?, notes?, …} (§17.2)
    std::optional<std::string> next_cursor;  // present ⇒ more pages (§10)

    static RetrieveResult from_json(const Json& j) {
        RetrieveResult r;
        const Json& arr = (j.is_object() && j.contains("hits")) ? j["hits"] : j;
        if (arr.is_array()) for (const auto& h : arr) r.hits.push_back(Hit::from_json(h));
        if (j.is_object()) {
            if (j.contains("usage")) r.usage = j["usage"];
            if (auto it = j.find("nextCursor"); it != j.end() && it->is_string())
                r.next_cursor = it->get<std::string>();
        }
        return r;
    }
};

// ── multimodal Content blocks (spec §4.8) ─────────────────────────────────────
// Helpers to build the tagged Content blocks that `embed`/`retrieve` accept for
// non-text modalities. Text is representable as a bare string, but these keep a
// mixed `inputs` array uniform.
namespace content {
[[nodiscard]] inline Json text(std::string s) {
    return Json{{"type", "text"}, {"text", std::move(s)}};
}
// `data` is base64 (spec §4.8); `mime` e.g. "image/png", "audio/wav".
[[nodiscard]] inline Json image(std::string mime, std::string data) {
    return Json{{"type", "image"}, {"mimeType", std::move(mime)}, {"data", std::move(data)}};
}
[[nodiscard]] inline Json audio(std::string mime, std::string data) {
    return Json{{"type", "audio"}, {"mimeType", std::move(mime)}, {"data", std::move(data)}};
}
[[nodiscard]] inline Json blob(std::string mime, std::string data) {
    return Json{{"type", "blob"}, {"mimeType", std::move(mime)}, {"data", std::move(data)}};
}
} // namespace content

} // namespace rcp
