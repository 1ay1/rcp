#pragma once
// rcp/federation.hpp — query every reachable RCP engine and fuse the results.
//
// A Federation holds N connected Clients (each an engine) and implements spec
// §16: fan a retrieve out to every engine that advertises `retrieve`,
// concurrently, then merge the per-engine ranked lists with Reciprocal Rank
// Fusion (RRF) — the interoperable default that needs only ranks, so it is
// robust across heterogeneous scorers.
//
// Discovery is the caller's job (build Clients from a registry or catalog/list);
// this facade owns fan-out + fusion. Each fused Hit carries its origin engine id
// in meta.engine, and the effective capability set is the union of the engines'.

#include <algorithm>
#include <future>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "rcp/client.hpp"
#include "rcp/fusion.hpp"
#include "rcp/protocol.hpp"
#include "rcp/selector.hpp"
#include "rcp/types.hpp"

namespace rcp {

// One engine in the federation: a connected client + a local label + a fusion
// weight (§16.1). `required` engines fail the whole query on error; others are
// skipped.
struct Engine {
    std::string id;
    Client      client;
    double      weight   = 1.0;
    bool        required = false;
};

// RRF tuning (spec §16.3). rrf_k damps the contribution of low ranks.
struct FusionConfig {
    double      rrf_k = 60.0;
    // Per-engine candidate multiplier: ask each engine for k * over_fetch hits
    // so fusion has headroom. Clamped to >= k.
    std::size_t over_fetch = 3;
};

class Federation {
public:
    Federation() = default;

    // Take ownership of a connected engine.
    Federation& add(Engine e) { engines_.push_back(std::move(e)); return *this; }
    Federation& add(std::string id, Client c, double weight = 1.0, bool required = false) {
        engines_.push_back(Engine{std::move(id), std::move(c), weight, required});
        return *this;
    }

    [[nodiscard]] std::size_t size() const noexcept { return engines_.size(); }

    // ── build from an rcp.json registry (§16.1) ──────────────────────────────────
    // Connect every engine in the registry EAGERLY (a federation holds live
    // clients). A `required` engine that fails to connect fails the whole build;
    // an optional engine that fails is skipped. Use Selector instead when you want
    // lazy connect / pick-one semantics.
    [[nodiscard]] static Result<Federation> from_registry(const Json& doc) {
        auto sel = Selector::from_registry(doc);
        if (!sel) return std::unexpected(sel.error());
        Federation fed;
        for (const auto& spec : sel->engines()) {
            auto client = spec.open();
            if (!client) {
                if (spec.required)
                    return fail<Federation>(client.error().code,
                        "required engine '" + spec.id + "' failed to connect: " + client.error().message);
                continue;
            }
            fed.add(spec.id, std::move(*client), spec.weight, spec.required);
        }
        if (fed.size() == 0)
            return fail<Federation>(errc::BackendUnavailable, "no federated engine could be connected");
        return fed;
    }

    [[nodiscard]] static Result<Federation> from_registry_file(const std::string& path) {
        auto sel = Selector::from_registry_file(path);
        if (!sel) return std::unexpected(sel.error());
        // Re-serialise specs through the doc path is overkill; connect directly.
        Federation fed;
        for (const auto& spec : sel->engines()) {
            auto client = spec.open();
            if (!client) {
                if (spec.required)
                    return fail<Federation>(client.error().code,
                        "required engine '" + spec.id + "' failed to connect: " + client.error().message);
                continue;
            }
            fed.add(spec.id, std::move(*client), spec.weight, spec.required);
        }
        if (fed.size() == 0)
            return fail<Federation>(errc::BackendUnavailable, "no federated engine could be connected");
        return fed;
    }

    // Effective capability set: the UNION across engines (§16.4). True if ANY
    // engine advertises `c`.
    [[nodiscard]] bool any_supports(Capability c) const noexcept {
        for (auto& e : engines_) if (e.client.supports(c)) return true;
        return false;
    }
    // Guaranteed set: the INTERSECTION. True only if EVERY engine advertises `c`.
    [[nodiscard]] bool all_support(Capability c) const noexcept {
        if (engines_.empty()) return false;
        for (auto& e : engines_) if (!e.client.supports(c)) return false;
        return true;
    }

    // Federated retrieve: fan out to every retrieve-capable engine concurrently,
    // then RRF-fuse. `k` is a refinement type (cannot be zero). `opts` is passed
    // through to each engine's retrieve unchanged.
    [[nodiscard]] Result<std::vector<Hit>>
    retrieve(std::string_view query, TopK k, Json opts = Json::object(), FusionConfig fc = {}) {
        const std::size_t want = k.get();
        const std::size_t per_engine = std::max(want, want * fc.over_fetch);

        // Fan out concurrently. Each future yields (engine index, result).
        struct EngineResult { std::size_t idx; Result<std::vector<Hit>> hits; };
        std::vector<std::future<EngineResult>> futs;
        for (std::size_t i = 0; i < engines_.size(); ++i) {
            if (!engines_[i].client.supports(Capability::Retrieve)) continue;
            futs.push_back(std::async(std::launch::async, [this, i, query, per_engine, opts]() mutable {
                auto tk = TopK::make(per_engine);
                // per_engine >= want >= 1, so make() always succeeds; guard anyway.
                if (!tk) return EngineResult{i, std::unexpected(tk.error())};
                return EngineResult{i, engines_[i].client.retrieve(query, *tk, opts)};
            }));
        }
        if (futs.empty())
            return fail<std::vector<Hit>>(errc::CapabilityMissing,
                                          "no federated engine advertises 'retrieve'");

        // Collect, honouring `required`.
        std::vector<std::pair<std::size_t, std::vector<Hit>>> lists;
        for (auto& f : futs) {
            EngineResult er = f.get();
            if (!er.hits) {
                if (engines_[er.idx].required)
                    return fail<std::vector<Hit>>(er.hits.error().code,
                        "required engine '" + engines_[er.idx].id + "' failed: " + er.hits.error().message);
                continue; // skip a failed optional engine
            }
            lists.emplace_back(er.idx, std::move(*er.hits));
        }
        if (lists.empty())
            return fail<std::vector<Hit>>(errc::BackendUnavailable, "all federated engines failed");

        return fuse_rrf(lists, want, fc);
    }

    // Access an individual engine (per-engine handle, §16.4).
    [[nodiscard]] Engine&       operator[](std::size_t i)       { return engines_[i]; }
    [[nodiscard]] const Engine& operator[](std::size_t i) const { return engines_[i]; }

private:
    // Reciprocal Rank Fusion (§16.3), delegated to the standalone, spec-exact
    // implementation in rcp/fusion.hpp — one source of truth, so the live
    // Federation and a manual fuse of held lists agree byte-for-byte (full
    // deterministic tie-break + richest-body dedup included).
    [[nodiscard]] Result<std::vector<Hit>>
    fuse_rrf(const std::vector<std::pair<std::size_t, std::vector<Hit>>>& lists,
             std::size_t want, const FusionConfig& fc) {
        std::vector<fusion::EngineList> engine_lists;
        engine_lists.reserve(lists.size());
        for (const auto& [engine_idx, hits] : lists) {
            engine_lists.push_back(fusion::EngineList{
                engines_[engine_idx].id, hits, engines_[engine_idx].weight});
        }
        return fusion::rrf_fuse(engine_lists, want, fc.rrf_k);
    }

    std::vector<Engine> engines_;
};

} // namespace rcp
