// rcp/fusion — federation fusion for RCP/1 (spec §16.3), the reference impl.
//
// The spec specifies exactly how to merge per-engine ranked lists into one
// list; this module is that algorithm as importable, tested code so no adopter
// re-derives it (and re-derives it subtly wrong). Two strategies, matching the
// Python/Rust/C++ SDKs byte-for-byte:
//
//   * Reciprocal Rank Fusion (RRF) — the default. Needs only ranks, so it is
//     robust across engines whose scores are not comparable.
//   * Weighted score fusion — for engines with comparable scores, with the
//     mandatory per-engine min-max normalisation applied first.
//
// Both use the spec's deterministic tie-break (§16.3): higher fused score, then
// higher summed weight, then lexicographically smaller stringified id — so
// fusion is reproducible regardless of the order engine responses arrive in.
//
// A "hit" is any object with an `id` (and, for weighted fusion, a numeric
// `score`); a "ranked list" is hits in descending relevance from `retrieve`.

export const RRF_K_DEFAULT = 60; // Cormack et al. 2009; damps low-rank influence.

// §16.3: on dedup keep the hit with the richest body (longest text, most blocks).
function richness(hit) {
  const t = typeof hit.text === "string" ? hit.text.length : 0;
  const c = Array.isArray(hit.content) ? hit.content.length : 0;
  return [t, c];
}

function richerThan(a, b) {
  const [at, ac] = richness(a);
  const [bt, bc] = richness(b);
  return at > bt || (at === bt && ac > bc);
}

// §16.3 deterministic total order over {fused, weight, id}: -fused, -weight, id↑.
function compareEntries(a, b) {
  if (a.fused !== b.fused) return b.fused - a.fused;
  if (a.weight !== b.weight) return b.weight - a.weight;
  return a.id < b.id ? -1 : a.id > b.id ? 1 : 0;
}

// engineLists: object OR Map of engineId -> ranked list of hits (dicts w/ id).
function entries(engineLists) {
  if (engineLists instanceof Map) return Array.from(engineLists.entries());
  return Object.entries(engineLists);
}

function accumulate(engineLists, weights, contribution) {
  const w = weights || {};
  const scores = new Map();
  const weightSum = new Map();
  const bestHit = new Map();
  const origin = new Map();

  for (const [engineId, hits] of entries(engineLists)) {
    const ew = Number(w[engineId] ?? 1.0);
    contribution(hits, ew, (key, delta, hit) => {
      scores.set(key, (scores.get(key) ?? 0) + delta);
      weightSum.set(key, (weightSum.get(key) ?? 0) + ew);
      const prev = bestHit.get(key);
      if (prev === undefined || richerThan(hit, prev)) {
        bestHit.set(key, hit);
        origin.set(key, engineId);
      }
    });
  }

  const ordered = Array.from(scores.keys())
    .map((id) => ({ id, fused: scores.get(id), weight: weightSum.get(id) }))
    .sort(compareEntries);

  return ordered.map(({ id, fused }) => {
    const hit = { ...bestHit.get(id) };
    const meta = { ...(hit.meta ?? {}) };
    if (meta.engine === undefined) meta.engine = origin.get(id);
    meta.fusedScore = fused;
    hit.meta = meta;
    return hit;
  });
}

// Reciprocal Rank Fusion over per-engine ranked lists (spec §16.3).
// RRF(d) = Σ_engine weight / (rrfK + rank_engine(d)), 1-based rank.
export function rrfFuse(engineLists, { k = null, rrfK = RRF_K_DEFAULT, weights = null } = {}) {
  const out = accumulate(engineLists, weights, (hits, _w, add) => {
    hits.forEach((hit, i) => {
      const rank = i + 1;
      add(String(hit.id), Number(_w) / (rrfK + rank), hit);
    });
  });
  return k != null ? out.slice(0, k) : out;
}

// Weighted score fusion with mandatory per-engine min-max normalisation (§16.3).
// Only valid when every hit carries a numeric `score`. Prefer rrfFuse unless
// engine scores are known comparable in kind.
export function weightedFuse(engineLists, { k = null, weights = null } = {}) {
  const out = accumulate(engineLists, weights, (hits, _w, add) => {
    const vals = hits.filter((h) => typeof h.score === "number").map((h) => Number(h.score));
    const lo = vals.length ? Math.min(...vals) : 0;
    const hi = vals.length ? Math.max(...vals) : 0;
    const span = hi - lo;
    for (const hit of hits) {
      const raw = Number(hit.score ?? 0);
      const norm = span === 0 ? 1.0 : (raw - lo) / span;
      add(String(hit.id), Number(_w) * norm, hit);
    }
  });
  return k != null ? out.slice(0, k) : out;
}
