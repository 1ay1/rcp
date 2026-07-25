//! fusion.rs — federation fusion for RCP/1 (spec §16.3), the reference impl.
//!
//! The spec specifies exactly how to merge per-engine ranked lists into one;
//! this module is that algorithm as importable, tested code so no adopter
//! re-derives it (and re-derives it subtly wrong). Two strategies, matching the
//! Python / Node / C++ SDKs byte-for-byte:
//!
//!   * Reciprocal Rank Fusion (RRF) — the default. Needs only ranks, so it is
//!     robust across engines whose scores are not comparable.
//!   * Weighted score fusion — for engines with comparable scores, with the
//!     mandatory per-engine min-max normalisation applied first.
//!
//! Both use the spec's deterministic tie-break (§16.3): higher fused score, then
//! higher summed weight, then lexicographically smaller stringified id — so
//! fusion is reproducible regardless of the order engine responses arrive in.
//!
//! A "hit" is a [`Json`] object with an `id` (and, for weighted fusion, a
//! numeric `score`); an engine list is `(engine_id, ranked hits descending)`.

use crate::json::Json;
use std::collections::HashMap;

/// Cormack et al. 2009; damps the influence of low ranks.
pub const RRF_K_DEFAULT: f64 = 60.0;

fn hit_id(hit: &Json) -> String {
    match hit.get("id") {
        Some(Json::Str(s)) => s.clone(),
        Some(Json::Int(n)) => n.to_string(),
        Some(Json::Float(f)) => f.to_string(),
        _ => String::new(),
    }
}

/// §16.3: on dedup keep the hit with the richest body (longest text, most blocks).
fn richness(hit: &Json) -> (usize, usize) {
    let t = hit.get("text").and_then(|v| v.as_str()).map_or(0, |s| s.len());
    let c = hit.get("content").and_then(|v| v.as_array()).map_or(0, |a| a.len());
    (t, c)
}

struct Acc {
    scores: HashMap<String, f64>,
    weight_sum: HashMap<String, f64>,
    best_hit: HashMap<String, Json>,
    origin: HashMap<String, String>,
}

impl Acc {
    fn new() -> Acc {
        Acc {
            scores: HashMap::new(),
            weight_sum: HashMap::new(),
            best_hit: HashMap::new(),
            origin: HashMap::new(),
        }
    }

    fn add(&mut self, engine: &str, weight: f64, hit: &Json, delta: f64) {
        let key = hit_id(hit);
        *self.scores.entry(key.clone()).or_insert(0.0) += delta;
        *self.weight_sum.entry(key.clone()).or_insert(0.0) += weight;
        let replace = match self.best_hit.get(&key) {
            None => true,
            Some(prev) => richness(hit) > richness(prev),
        };
        if replace {
            self.best_hit.insert(key.clone(), hit.clone());
            self.origin.insert(key, engine.to_string());
        }
    }

    /// Emit fused hits in the spec's deterministic total order.
    fn finish(self, k: Option<usize>) -> Vec<Json> {
        let mut keys: Vec<String> = self.scores.keys().cloned().collect();
        keys.sort_by(|a, b| {
            let (fa, fb) = (self.scores[a], self.scores[b]);
            // -fused
            fb.partial_cmp(&fa)
                .unwrap_or(std::cmp::Ordering::Equal)
                // -weight
                .then_with(|| {
                    self.weight_sum[b]
                        .partial_cmp(&self.weight_sum[a])
                        .unwrap_or(std::cmp::Ordering::Equal)
                })
                // id ascending
                .then_with(|| a.cmp(b))
        });
        if let Some(k) = k {
            keys.truncate(k);
        }
        keys.into_iter()
            .map(|key| {
                let mut hit = self.best_hit[&key].clone();
                let mut meta = hit.get("meta").cloned().unwrap_or_else(Json::object);
                if meta.get("engine").is_none() {
                    meta.insert("engine", self.origin[&key].as_str());
                }
                meta.insert("fusedScore", self.scores[&key]);
                hit.insert("meta", meta);
                hit
            })
            .collect()
    }
}

/// Reciprocal Rank Fusion over per-engine ranked lists (spec §16.3).
///
/// `RRF(d) = Σ_engine weight / (rrf_k + rank_engine(d))`, 1-based rank. `weights`
/// maps engine id → weight (defaulting to 1.0). Pass `k` to truncate the result.
///
/// ```
/// use rcp::{fusion, obj};
/// let a = vec![obj(&[("id", "a".into())]), obj(&[("id", "b".into())])];
/// let b = vec![obj(&[("id", "b".into())]), obj(&[("id", "d".into())])];
/// let fused = fusion::rrf_fuse(&[("A", &a), ("B", &b)], None, 60.0, None);
/// assert_eq!(fused[0].get_str("id"), Some("b")); // in both lists
/// ```
pub fn rrf_fuse(
    engine_lists: &[(&str, &Vec<Json>)],
    k: Option<usize>,
    rrf_k: f64,
    weights: Option<&HashMap<String, f64>>,
) -> Vec<Json> {
    let mut acc = Acc::new();
    for (engine, hits) in engine_lists {
        let w = weights.and_then(|m| m.get(*engine)).copied().unwrap_or(1.0);
        for (i, hit) in hits.iter().enumerate() {
            let rank = (i + 1) as f64;
            acc.add(engine, w, hit, w / (rrf_k + rank));
        }
    }
    acc.finish(k)
}

/// Weighted score fusion with mandatory per-engine min-max normalisation (§16.3).
///
/// Only valid when every hit carries a numeric `score`. Prefer [`rrf_fuse`]
/// unless engine scores are known comparable in kind.
pub fn weighted_fuse(
    engine_lists: &[(&str, &Vec<Json>)],
    k: Option<usize>,
    weights: Option<&HashMap<String, f64>>,
) -> Vec<Json> {
    let mut acc = Acc::new();
    for (engine, hits) in engine_lists {
        let w = weights.and_then(|m| m.get(*engine)).copied().unwrap_or(1.0);
        let vals: Vec<f64> = hits.iter().filter_map(|h| h.get("score").and_then(|v| v.as_f64())).collect();
        let lo = vals.iter().cloned().fold(f64::INFINITY, f64::min);
        let hi = vals.iter().cloned().fold(f64::NEG_INFINITY, f64::max);
        let span = if vals.is_empty() { 0.0 } else { hi - lo };
        for hit in hits.iter() {
            let raw = hit.get("score").and_then(|v| v.as_f64()).unwrap_or(0.0);
            let norm = if span == 0.0 { 1.0 } else { (raw - lo) / span };
            acc.add(engine, w, hit, w * norm);
        }
    }
    acc.finish(k)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::json::obj;

    fn ids(hits: &[Json]) -> Vec<String> {
        hits.iter().map(|h| h.get_str("id").unwrap().to_string()).collect()
    }

    #[test]
    fn rrf_matches_hand_computation() {
        // A=[a,b,c], B=[b,d], rrf_k=60. b=1/62+1/61, a=1/61, d=1/62, c=1/63.
        let a = vec![obj(&[("id", "a".into())]), obj(&[("id", "b".into())]), obj(&[("id", "c".into())])];
        let b = vec![obj(&[("id", "b".into())]), obj(&[("id", "d".into())])];
        let fused = rrf_fuse(&[("A", &a), ("B", &b)], None, 60.0, None);
        assert_eq!(ids(&fused), vec!["b", "a", "d", "c"]);
        let b_score = 1.0 / 62.0 + 1.0 / 61.0;
        let got = fused[0].get("meta").unwrap().get("fusedScore").unwrap().as_f64().unwrap();
        assert!((got - b_score).abs() < 1e-12);
        assert_eq!(fused[0].get("meta").unwrap().get_str("engine"), Some("A"));
    }

    #[test]
    fn k_truncates() {
        let a = vec![obj(&[("id", "a".into())]), obj(&[("id", "b".into())]), obj(&[("id", "c".into())])];
        assert_eq!(rrf_fuse(&[("A", &a)], Some(2), 60.0, None).len(), 2);
    }

    #[test]
    fn tie_break_is_id_ascending() {
        let x = vec![obj(&[("id", "z".into())])];
        let y = vec![obj(&[("id", "a".into())])];
        let fused = rrf_fuse(&[("X", &x), ("Y", &y)], None, 60.0, None);
        assert_eq!(ids(&fused), vec!["a", "z"]);
    }

    #[test]
    fn weighted_normalises_per_engine() {
        let a = vec![obj(&[("id", "a".into()), ("score", 10.0.into())]), obj(&[("id", "b".into()), ("score", 0.0.into())])];
        let b = vec![obj(&[("id", "b".into()), ("score", 5.0.into())])];
        let fused = weighted_fuse(&[("A", &a), ("B", &b)], None, None);
        // a->1.0, b->0.0+1.0=1.0. Fused tie -> higher weight (b, weightSum 2) first.
        assert_eq!(ids(&fused), vec!["b", "a"]);
    }

    #[test]
    fn dedup_keeps_richest_body() {
        let a = vec![obj(&[("id", "d".into()), ("text", "short".into())])];
        let b = vec![obj(&[("id", "d".into()), ("text", "a much longer body".into())])];
        let fused = rrf_fuse(&[("A", &a), ("B", &b)], None, 60.0, None);
        assert_eq!(fused.len(), 1);
        assert_eq!(fused[0].get_str("text"), Some("a much longer body"));
    }
}
