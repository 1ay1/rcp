#pragma once
// rcp/fusion.hpp — federation fusion for RCP/1 (spec §16.3), reusable & pure.
//
// The Federation facade (rcp/federation.hpp) fuses LIVE engine responses; this
// header is the same algorithm as a standalone, testable function over ranked
// lists you already hold — so an adopter with hits from any source (a cache, a
// replay, a local index) fuses them without a live Federation, matching the
// Python / Node / Rust SDKs byte-for-byte.
//
// Two strategies:
//   * rrf_fuse       — Reciprocal Rank Fusion, the default. Needs only ranks.
//   * weighted_fuse  — weighted score fusion, with mandatory per-engine min-max
//                      normalisation first (only valid for comparable scores).
//
// Both apply the spec's DETERMINISTIC total order (§16.3): higher fused score,
// then higher summed weight, then lexicographically smaller id — so fusion is
// reproducible regardless of the order engine responses arrive in. On duplicate
// ids the richest body (longest text, then most content blocks) is kept.

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "rcp/protocol.hpp"
#include "rcp/types.hpp"

namespace rcp::fusion {

// Cormack et al. 2009; damps the influence of low ranks.
inline constexpr double kRrfKDefault = 60.0;

// One engine's contribution to a fuse: a label + its ranked hits (descending) +
// a fusion weight (§16.1).
struct EngineList {
    std::string      id;
    std::vector<Hit> hits;
    double           weight = 1.0;
};

namespace detail {

// §16.3 richness for dedup: longer text wins, then more content blocks.
inline std::pair<std::size_t, std::size_t> richness(const Hit& h) {
    std::size_t blocks = h.content.is_array() ? h.content.size() : 0;
    return {h.text.size(), blocks};
}

struct Acc {
    double      fused  = 0.0;
    double      weight = 0.0;
    Hit         hit;
    std::string engine;
};

// Assemble the accumulator map into the spec's deterministic total order,
// stamping meta.engine (origin) and meta.fusedScore, truncated to `k` if set.
inline std::vector<Hit> finish(std::unordered_map<std::string, Acc>& acc,
                               std::optional<std::size_t> k) {
    std::vector<Acc> merged;
    merged.reserve(acc.size());
    for (auto& [id, a] : acc) merged.push_back(std::move(a));

    std::sort(merged.begin(), merged.end(), [](const Acc& a, const Acc& b) {
        if (a.fused != b.fused) return a.fused > b.fused;      // -fused
        if (a.weight != b.weight) return a.weight > b.weight;  // -weight
        return a.hit.id < b.hit.id;                            // id ascending
    });

    const std::size_t n = k ? std::min(*k, merged.size()) : merged.size();
    std::vector<Hit> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        Hit h = std::move(merged[i].hit);
        h.score = Score{merged[i].fused};
        if (!h.meta.is_object()) h.meta = Json::object();
        if (!h.meta.contains("engine")) h.meta["engine"] = merged[i].engine;
        h.meta["fusedScore"] = merged[i].fused;
        out.push_back(std::move(h));
    }
    return out;
}

// Fold one (hit, delta, weight, engine) contribution into the accumulator,
// keeping the richest body seen for that id.
inline void add(std::unordered_map<std::string, Acc>& acc, const Hit& h,
                double delta, double weight, const std::string& engine) {
    auto it = acc.find(h.id);
    if (it == acc.end()) {
        Acc a;
        a.fused = delta;
        a.weight = weight;
        a.hit = h;
        a.engine = engine;
        acc.emplace(h.id, std::move(a));
    } else {
        it->second.fused += delta;
        it->second.weight += weight;
        if (richness(h) > richness(it->second.hit)) {
            it->second.hit = h;
            it->second.engine = engine;
        }
    }
}

} // namespace detail

// Reciprocal Rank Fusion over per-engine ranked lists (spec §16.3).
// RRF(d) = Σ_engine weight / (rrf_k + rank_engine(d)), 1-based rank.
[[nodiscard]] inline std::vector<Hit>
rrf_fuse(const std::vector<EngineList>& engines,
         std::optional<std::size_t> k = std::nullopt,
         double rrf_k = kRrfKDefault) {
    std::unordered_map<std::string, detail::Acc> acc;
    for (const auto& e : engines) {
        for (std::size_t rank = 0; rank < e.hits.size(); ++rank) {
            double contrib = e.weight / (rrf_k + static_cast<double>(rank + 1));
            detail::add(acc, e.hits[rank], contrib, e.weight, e.id);
        }
    }
    return detail::finish(acc, k);
}

// Weighted score fusion with mandatory per-engine min-max normalisation (§16.3).
// Only valid when every hit carries a comparable `score`; prefer rrf_fuse
// otherwise.
[[nodiscard]] inline std::vector<Hit>
weighted_fuse(const std::vector<EngineList>& engines,
              std::optional<std::size_t> k = std::nullopt) {
    std::unordered_map<std::string, detail::Acc> acc;
    for (const auto& e : engines) {
        double lo = 0.0, hi = 0.0;
        bool first = true;
        for (const auto& h : e.hits) {
            double s = h.score.get();
            if (first) { lo = hi = s; first = false; }
            else { lo = std::min(lo, s); hi = std::max(hi, s); }
        }
        const double span = hi - lo;
        for (const auto& h : e.hits) {
            double norm = span == 0.0 ? 1.0 : (h.score.get() - lo) / span;
            detail::add(acc, h, e.weight * norm, e.weight, e.id);
        }
    }
    return detail::finish(acc, k);
}

} // namespace rcp::fusion
