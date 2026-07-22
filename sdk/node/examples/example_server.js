#!/usr/bin/env node
// example_server.js — a complete, dependency-free RCP/1 retrieval engine.
//
// Run as a subprocess (stdio, the default):  node examples/example_server.js
// Or over HTTP:                              node examples/example_server.js --http 8000
//
// It advertises embed + retrieve + graph over a toy in-memory index. Swap the
// handler bodies for a real embedder / vector store / graph — the wire contract
// (JSON-RPC over stdio or HTTP) is all a client ever sees.
import * as rcp from "../src/index.js";

const DIM = 384;
const DOCS = [
  ["d1", "The Eiffel Tower is a wrought-iron lattice tower in Paris, France."],
  ["d2", "Photosynthesis converts light energy into chemical energy in plants."],
  ["d3", "The Great Wall of China stretches thousands of kilometres."],
];

function embedOne(text) {
  const v = new Array(DIM).fill(0);
  for (const tok of text.toLowerCase().split(/\s+/)) {
    if (!tok) continue;
    let h = 2166136261 >>> 0; // FNV-1a (32-bit)
    for (let i = 0; i < tok.length; i++) {
      h ^= tok.charCodeAt(i);
      h = Math.imul(h, 16777619) >>> 0;
    }
    v[h % DIM] += 1.0;
  }
  let n = 0;
  for (const x of v) n += x * x;
  n = n > 0 ? Math.sqrt(n) : 1.0;
  return v.map((x) => x / n);
}

function cosine(a, b) {
  let s = 0;
  for (let i = 0; i < a.length; i++) s += a[i] * b[i];
  return s;
}

const INDEX = DOCS.map(([id, text]) => ({ id, text, vec: embedOne(text) }));

function search(query, k) {
  const q = embedOne(query);
  return INDEX.map((d) => ({ id: d.id, score: cosine(q, d.vec), text: d.text }))
    .sort((a, b) => b.score - a.score)
    .slice(0, k);
}

function build() {
  const s = new rcp.Server();
  s.setInfo("rcp-example", "1.0.0");
  s.advertise(rcp.Capability.Embed, { dimensions: DIM, modes: ["dense"] });
  s.advertise(rcp.Capability.Retrieve, { maxK: 100, modes: ["dense"], citations: true });
  s.advertise(rcp.Capability.Graph, { ops: ["local", "global"] });

  s.on(rcp.Method.EMBED, (params) => {
    // Accept `inputs` (preferred, spec §7.3) or the legacy `texts` synonym.
    const items = params.inputs || params.texts || [];
    return { vectors: items.map(embedOne), dimensions: DIM };
  });

  s.on(rcp.Method.RETRIEVE, (params) => ({
    hits: search(params.query || "", Number(params.k ?? 10)),
  }));

  s.on(rcp.Method.GRAPH, (params) => {
    const op = params.op || "local";
    if (op === "local") return { hits: search(params.query || "", Number(params.k ?? 5)) };
    if (op === "global") return { summary: "a global community summary would go here", communities: 1 };
    return { op, note: "unimplemented" };
  });

  return s;
}

const srv = build();
const args = process.argv.slice(2);
if (args[0] === "--http") {
  await srv.serveHttp(Number(args[1]));
} else {
  await srv.serveStdio();
}
