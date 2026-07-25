"""Federation fusion for RCP/1 (spec §16.3) — the reference implementation.

The spec specifies exactly how to merge per-engine ranked lists into one; this
module is that algorithm as importable, tested code so no adopter re-derives it
(and re-derives it subtly wrong). Two strategies, matching the spec:

  * Reciprocal Rank Fusion (RRF) — the default. Needs only ranks, so it is
    robust across engines whose scores are not comparable.
  * Weighted score fusion — for engines that expose comparable scores, with the
    mandatory per-engine min-max normalisation applied first.

Both use the spec's deterministic tie-break (§16.3): higher fused score, then
higher summed weight, then lexicographically smaller stringified id — so fusion
is reproducible regardless of the order engine responses arrive in.

Dependency-free (stdlib only). A "hit" is any dict with an ``id`` (and, for
weighted fusion, a numeric ``score``); a "ranked list" is hits in descending
relevance as returned by ``retrieve``.
"""
from __future__ import annotations

RRF_K_DEFAULT = 60  # Cormack et al. 2009; damps the influence of low ranks.


def _sort_key(entry):
    # §16.3 deterministic total order: -fused, -weight, then id ascending.
    fused, weight, sid = entry
    return (-fused, -weight, sid)


def rrf_fuse(engine_lists, k=None, rrf_k=RRF_K_DEFAULT, weights=None):
    """Reciprocal Rank Fusion over per-engine ranked lists (spec §16.3).

    ``engine_lists`` maps an engine id → its ranked list of hits (dicts with an
    ``id``). ``weights`` optionally maps engine id → weight (default 1.0).
    Returns one fused list of hits, sorted and truncated to ``k`` (all if None).
    Each fused hit gains ``meta.engine`` (origin) and ``meta.engineRank``.

    ``RRF(d) = Σ_engine  weight / (rrf_k + rank_engine(d))``  with 1-based rank.
    """
    weights = weights or {}
    scores: dict[str, float] = {}
    weight_sum: dict[str, float] = {}
    best_hit: dict[str, dict] = {}
    origin: dict[str, str] = {}

    for engine_id, hits in engine_lists.items():
        w = float(weights.get(engine_id, 1.0))
        for rank, hit in enumerate(hits, start=1):
            key = str(hit["id"])
            scores[key] = scores.get(key, 0.0) + w / (rrf_k + rank)
            weight_sum[key] = weight_sum.get(key, 0.0) + w
            # §16.3 dedup: keep the richest body across engines.
            prev = best_hit.get(key)
            if prev is None or _richness(hit) > _richness(prev):
                best_hit[key] = hit
                origin[key] = engine_id

    ordered = sorted(
        ((scores[key], weight_sum[key], key) for key in scores),
        key=_sort_key,
    )
    out = []
    for fused, _w, key in ordered:
        hit = dict(best_hit[key])
        meta = dict(hit.get("meta") or {})
        meta.setdefault("engine", origin[key])
        meta["fusedScore"] = fused
        hit["meta"] = meta
        out.append(hit)
    return out[:k] if k is not None else out


def weighted_fuse(engine_lists, k=None, weights=None):
    """Weighted score fusion with mandatory per-engine min-max normalisation
    (spec §16.3). Only valid when every hit carries a numeric ``score``.

    ``Fused(d) = Σ_engine  weight · norm_engine(score_engine(d))`` where
    ``norm`` is min-max over that engine's returned set. Prefer ``rrf_fuse``
    unless the engines' scores are known to be comparable in kind.
    """
    weights = weights or {}
    scores: dict[str, float] = {}
    weight_sum: dict[str, float] = {}
    best_hit: dict[str, dict] = {}
    origin: dict[str, str] = {}

    for engine_id, hits in engine_lists.items():
        w = float(weights.get(engine_id, 1.0))
        vals = [float(h["score"]) for h in hits if "score" in h]
        lo, hi = (min(vals), max(vals)) if vals else (0.0, 0.0)
        span = hi - lo
        for hit in hits:
            key = str(hit["id"])
            raw = float(hit.get("score", 0.0))
            norm = 1.0 if span == 0 else (raw - lo) / span
            scores[key] = scores.get(key, 0.0) + w * norm
            weight_sum[key] = weight_sum.get(key, 0.0) + w
            prev = best_hit.get(key)
            if prev is None or _richness(hit) > _richness(prev):
                best_hit[key] = hit
                origin[key] = engine_id

    ordered = sorted(
        ((scores[key], weight_sum[key], key) for key in scores),
        key=_sort_key,
    )
    out = []
    for fused, _w, key in ordered:
        hit = dict(best_hit[key])
        meta = dict(hit.get("meta") or {})
        meta.setdefault("engine", origin[key])
        meta["fusedScore"] = fused
        hit["meta"] = meta
        out.append(hit)
    return out[:k] if k is not None else out


def _richness(hit):
    """§16.3: on dedup keep the hit with the richest body (longest text / most
    content blocks)."""
    return (len(hit.get("text") or ""), len(hit.get("content") or []))
