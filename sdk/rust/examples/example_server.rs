//! example_server.rs — a complete, dependency-free RCP/1 retrieval engine.
//!
//! Run over stdio (default):  cargo run --example example_server
//! Or over HTTP:              cargo run --example example_server -- --http 8000

use rcp::{obj, Capability, Json, Method, Server};

const DIM: usize = 384;

const DOCS: [(&str, &str); 3] = [
    ("d1", "The Eiffel Tower is a wrought-iron lattice tower in Paris, France."),
    ("d2", "Photosynthesis converts light energy into chemical energy in plants."),
    ("d3", "The Great Wall of China stretches thousands of kilometres."),
];

fn embed_one(text: &str) -> Vec<f64> {
    let mut v = vec![0.0f64; DIM];
    for tok in text.to_lowercase().split_whitespace() {
        let mut h: u32 = 2166136261; // FNV-1a (32-bit)
        for b in tok.bytes() {
            h ^= b as u32;
            h = h.wrapping_mul(16777619);
        }
        v[(h as usize) % DIM] += 1.0;
    }
    let norm = v.iter().map(|x| x * x).sum::<f64>().sqrt();
    let norm = if norm > 0.0 { norm } else { 1.0 };
    v.iter().map(|x| x / norm).collect()
}

fn cosine(a: &[f64], b: &[f64]) -> f64 {
    a.iter().zip(b).map(|(x, y)| x * y).sum()
}

fn search(query: &str, k: usize) -> Json {
    let q = embed_one(query);
    let mut scored: Vec<(f64, &str, &str)> = DOCS
        .iter()
        .map(|(id, text)| (cosine(&q, &embed_one(text)), *id, *text))
        .collect();
    scored.sort_by(|a, b| b.0.partial_cmp(&a.0).unwrap_or(std::cmp::Ordering::Equal));
    let hits: Vec<Json> = scored
        .iter()
        .take(k)
        .map(|(s, id, text)| {
            obj(&[
                ("id", (*id).into()),
                ("score", (*s).into()),
                ("text", (*text).into()),
            ])
        })
        .collect();
    Json::Array(hits)
}

fn build() -> Server {
    let mut s = Server::new();
    s.set_info("rcp-example", "1.0.0");
    s.advertise(
        Capability::Embed,
        obj(&[("dimensions", (DIM as i64).into()), ("modes", vec![Json::from("dense")].into())]),
    );
    s.advertise(
        Capability::Retrieve,
        obj(&[
            ("maxK", 100.into()),
            ("modes", vec![Json::from("dense")].into()),
            ("citations", true.into()),
        ]),
    );
    s.advertise(
        Capability::Graph,
        obj(&[("ops", vec![Json::from("local"), Json::from("global")].into())]),
    );

    s.on(Method::EMBED, |params| {
        // Accept `inputs` (preferred, spec §7.3) or the legacy `texts` synonym.
        let items = params
            .get("inputs")
            .or_else(|| params.get("texts"))
            .and_then(|v| v.as_array())
            .cloned()
            .unwrap_or_default();
        let vectors: Vec<Json> = items
            .iter()
            .filter_map(|x| x.as_str())
            .map(|t| Json::from(embed_one(t)))
            .collect();
        Ok(obj(&[("vectors", Json::Array(vectors)), ("dimensions", (DIM as i64).into())]))
    });

    s.on(Method::RETRIEVE, |params| {
        let q = params.get_str("query").unwrap_or("");
        let k = params.get("k").and_then(|v| v.as_i64()).unwrap_or(10).max(0) as usize;
        Ok(obj(&[("hits", search(q, k))]))
    });

    s.on(Method::GRAPH, |params| {
        let op = params.get_str("op").unwrap_or("local");
        match op {
            "local" => {
                let q = params.get_str("query").unwrap_or("");
                let k = params.get("k").and_then(|v| v.as_i64()).unwrap_or(5).max(0) as usize;
                Ok(obj(&[("hits", search(q, k))]))
            }
            "global" => Ok(obj(&[
                ("summary", "a global community summary would go here".into()),
                ("communities", 1.into()),
            ])),
            other => Ok(obj(&[("op", other.into()), ("note", "unimplemented".into())])),
        }
    });

    s
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let mut s = build();
    if args.len() >= 3 && args[1] == "--http" {
        let port: u16 = args[2].parse().unwrap_or(8000);
        let _ = s.serve_http(port);
    } else {
        s.serve_stdio();
    }
}
