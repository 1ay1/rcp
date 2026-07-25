// test.js — smoke test for the native Node.js RCP SDK: client + server + gating.
//
// Zero dependencies. The `client<->C++` case proves cross-language interop
// against sdk/cpp/example_server when it is built.
import assert from "node:assert/strict";
import { existsSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import * as rcp from "./src/index.js";

const here = path.dirname(fileURLToPath(import.meta.url));

async function testServerInproc() {
  // Exercise the Server dispatcher in-process (no transport).
  const s = new rcp.Server();
  s.setInfo("node-engine", "1.0");
  s.advertise(rcp.Capability.Retrieve, { maxK: 100, modes: ["hybrid"] });
  s.on("retrieve", (params) => ({ hits: [{ id: "d1", score: 0.9, text: params.query }] }));

  // pre-initialize -> NotInitialized
  let r = await s.handle({ jsonrpc: "2.0", id: 1, method: "retrieve", params: { query: "x", k: 1 } });
  assert.equal(r.error.code, -32001, JSON.stringify(r));

  const init = await s.handle({ jsonrpc: "2.0", id: 2, method: "initialize", params: { protocolVersion: 1 } });
  const caps = init.result.capabilities;
  assert.ok("retrieve" in caps && !("embed" in caps), JSON.stringify(caps));

  const ret = await s.handle({ jsonrpc: "2.0", id: 3, method: "retrieve", params: { query: "hello", k: 1 } });
  assert.equal(ret.result.hits[0].text, "hello", JSON.stringify(ret));

  // unadvertised capability -> CapabilityMissing
  const emb = await s.handle({ jsonrpc: "2.0", id: 4, method: "embed", params: { texts: ["a"] } });
  assert.equal(emb.error.code, -32003, JSON.stringify(emb));

  // unknown method -> UnknownMethod
  const unk = await s.handle({ jsonrpc: "2.0", id: 5, method: "no/such", params: {} });
  assert.equal(unk.error.code, -32004, JSON.stringify(unk));

  // batch: one response per request, notification drops out
  const batch = await s.handleLine(
    JSON.stringify([
      { jsonrpc: "2.0", id: 6, method: "ping", params: { nonce: 1 } },
      { jsonrpc: "2.0", method: "notifications/cancel", params: { id: 6 } },
      { jsonrpc: "2.0", id: 7, method: "info", params: {} },
    ])
  );
  const arr = JSON.parse(batch);
  assert.equal(arr.length, 2, batch);
  assert.equal(arr[0].result.nonce, 1, batch);

  console.log("server in-proc: ok");
}

async function testClientAgainstCppServer() {
  // Node client -> C++ example server subprocess (cross-language interop).
  const cpp = path.join(here, "..", "cpp", "example_server");
  if (!existsSync(cpp)) {
    console.log("client<->C++: skipped (build sdk/cpp/example_server first)");
    return;
  }
  const c = await rcp.connectStdio([cpp]);
  assert.equal(c.protocolVersion, 1);
  assert.ok(c.supports(rcp.Capability.Retrieve));
  assert.ok(!c.supports(rcp.Capability.Rerank));

  const hits = await c.retrieve("landmark in the French capital", 2);
  assert.equal(hits.length, 2);
  assert.ok(hits[0].id, JSON.stringify(hits));
  // the C++ server now returns agentic/eval fields — assert they cross the wire
  // and survive the (non-lossy) client normalisation.
  assert.ok(typeof hits[0].confidence === "number" && hits[0].confidence >= 0 && hits[0].confidence <= 1, JSON.stringify(hits[0]));
  assert.equal(hits[0].unit, "chunk", JSON.stringify(hits[0]));
  assert.equal(hits[0].trust?.level, "trusted", JSON.stringify(hits[0]));
  console.log(`client<->C++: ok, top hit = ${hits[0].id}`);

  // feedback + memory round-trip across languages against the C++ server
  if (c.supports(rcp.Capability.Feedback)) {
    const fb = await c.feedback([{ hitId: hits[0].id, used: true, reward: 0.9 }], { query: "q" });
    assert.equal(fb.accepted, 1, JSON.stringify(fb));
    console.log("client<->C++ feedback: ok");
  }
  if (c.supports(rcp.Capability.Memory)) {
    const mr = await c.memoryRecall("french landmark", { n: 2 });
    assert.ok(Array.isArray(mr.clues) && mr.clues.length > 0, JSON.stringify(mr));
    console.log("client<->C++ memory: ok");
  }

  // ping echoes the nonce (ungated, works any time)
  const pong = await c.ping(123);
  assert.equal(pong.nonce, 123, JSON.stringify(pong));
  console.log("ping: ok");

  // capability gating throws client-side for an unadvertised method
  await assert.rejects(
    () => c.rerank("q", ["a", "b"]),
    (e) => e instanceof rcp.RcpError && /rerank/.test(e.message),
  );
  console.log("client gating: ok");

  await c.shutdown();
}

async function testRegistrySelector() {
  // Selector.loads builds from an rcp.json registry and picks a live backend.
  const srv = path.join(here, "examples", "example_server.js");
  const reg = JSON.stringify({
    engines: [{ id: "docs", transport: "stdio", command: ["node", srv], priority: 10 }],
  });
  const sel = rcp.Selector.loads(reg);
  assert.equal(sel.size, 1);
  const c = await sel.selectPrimary();
  assert.ok(c.supports(rcp.Capability.Retrieve));
  const hits = await c.retrieve("french landmark", 1);
  assert.equal(hits.length, 1, JSON.stringify(hits));
  await c.shutdown();
  console.log("registry selector: ok");
}

async function testAgenticSurfaces() {
  // feedback + memory dispatch, capability gating, and full Hit-field round-trip.
  const s = new rcp.Server();
  s.setInfo("node-agentic", "1.0");
  s.advertise(rcp.Capability.Retrieve, { maxK: 100, confidence: true, units: ["chunk", "tree-node"] });
  s.advertise(rcp.Capability.Session, { dedup: true });
  s.advertise(rcp.Capability.Feedback, {});
  s.advertise(rcp.Capability.Memory, { scopes: ["global"], clues: true });

  s.on("retrieve", (params) => ({
    hits: [{
      id: 42, score: 0.9, text: params.query,
      confidence: 0.83, unit: params.unit ?? "chunk", level: params.level ?? 0,
      provenance: { path: ["e:a", "e:b"], leaves: ["doc:1#0"] },
      trust: { level: "trusted", injectionSuspected: false },
      scores: { dense: 0.7, rerank: 0.9 },
    }],
    usage: { tokens: 128 },
  }));
  s.on("feedback", (params) => ({ accepted: (params.signals ?? []).length }));
  s.on("memory/build", () => ({ memoryId: "mem-1", tokens: 2048 }));
  s.on("memory/recall", () => ({ clues: [{ query: "sub-q", seedIds: ["e:a"], weight: 0.9 }] }));

  await s.handle({ jsonrpc: "2.0", id: 1, method: "initialize", params: { protocolVersion: 1 } });

  // retrieve carries the agentic knobs; new hit fields survive normalisation
  const ret = await s.handle({
    jsonrpc: "2.0", id: 2, method: "retrieve",
    params: { query: "q", k: 1, unit: "tree-node", level: 2, tokenBudget: 500, sessionId: "traj-1" },
  });
  const h = ret.result.hits[0];
  assert.equal(h.confidence, 0.83, JSON.stringify(h));
  assert.equal(h.unit, "tree-node");
  assert.deepEqual(h.provenance.path, ["e:a", "e:b"]);
  assert.equal(h.trust.level, "trusted");
  assert.equal(h.scores.dense, 0.7);

  // feedback
  const fb = await s.handle({
    jsonrpc: "2.0", id: 3, method: "feedback",
    params: { sessionId: "traj-1", query: "q", signals: [{ hitId: "42", used: true, cited: true, reward: 0.8 }] },
  });
  assert.equal(fb.result.accepted, 1, JSON.stringify(fb));

  // memory build + recall
  const mb = await s.handle({ jsonrpc: "2.0", id: 4, method: "memory/build", params: { scope: "global" } });
  assert.equal(mb.result.memoryId, "mem-1", JSON.stringify(mb));
  const mr = await s.handle({ jsonrpc: "2.0", id: 5, method: "memory/recall", params: { query: "q", memoryId: "mem-1", n: 3 } });
  assert.deepEqual(mr.result.clues[0].seedIds, ["e:a"], JSON.stringify(mr));

  // a server that never advertised memory rejects it with CapabilityMissing
  const bare = new rcp.Server();
  bare.setInfo("bare", "1.0");
  bare.advertise(rcp.Capability.Retrieve, {});
  await bare.handle({ jsonrpc: "2.0", id: 1, method: "initialize", params: { protocolVersion: 1 } });
  const miss = await bare.handle({ jsonrpc: "2.0", id: 2, method: "memory/recall", params: { query: "q" } });
  assert.equal(miss.error.code, -32003, JSON.stringify(miss));

  console.log("agentic surfaces (feedback/memory/hit-fields): ok");
}

async function testFilter() {
  const f = rcp.filter;
  // Builder produces the exact spec §8 wire shape.
  assert.deepEqual(f.eq("lang", "fr").toJSON(), { field: "lang", op: "eq", value: "fr" });
  const tree = f.all(f.eq("lang", "en"), f.gte("year", 2015)).toJSON();
  assert.deepEqual(tree, {
    and: [
      { field: "lang", op: "eq", value: "en" },
      { field: "year", op: "gte", value: 2015 },
    ],
  });
  // Operator overloads compose.
  assert.deepEqual(
    f.eq("lang", "en").and(f.gte("year", 2015)).toJSON(),
    tree,
  );

  // Validator accepts a well-formed tree against advertised fields.
  const fields = { year: "int", lang: "keyword" };
  assert.deepEqual(f.validate(tree, { fields }), tree);
  assert.equal(f.validate(null, { fields }), null);
  assert.equal(f.validate({}, { fields }), null);

  // Validator rejects every malformed / unauthorized tree with -32602.
  const bad = [
    { field: "author", op: "eq", value: "z" }, // unadvertised field
    { field: "lang", op: "equals", value: "en" }, // typo op
    { and: "not-a-list" }, // malformed combinator
    { field: "year", op: "in", value: 2017 }, // in wants an array
    { or: [{ field: "lang" }] }, // leaf missing op
    { field: "lang", op: "gt", value: "en" }, // ordered op on keyword
    { field: "x", op: "eq", value: 1, and: [] }, // combinator+leaf mix
  ];
  for (const b of bad) {
    assert.throws(
      () => f.validate(b, { fields }),
      (e) => e instanceof rcp.RcpError && e.code === -32602 && e.data && "field" in e.data,
      `expected -32602 for ${JSON.stringify(b)}`,
    );
  }

  // Builder rejects nonsense locally, before the wire.
  assert.throws(() => f.eq("", "x"), TypeError);
  assert.throws(() => f.all("not-a-filter"), TypeError);

  console.log("filter builder + validator: ok");
}

function testFusion() {
  // RRF vs hand-computation. Engines A=[a,b,c], B=[b,d], rrfK=60.
  //   b = 1/62 (rank2 in A) + 1/61 (rank1 in B),  a = 1/61,  d = 1/62,  c = 1/63.
  //   0.03252 > 0.01639 > 0.01613 > 0.01587  ->  order [b,a,d,c].
  const A = [{ id: "a" }, { id: "b" }, { id: "c" }];
  const B = [{ id: "b" }, { id: "d" }];
  const fused = rcp.rrfFuse({ A, B }, { rrfK: 60 });
  assert.deepEqual(fused.map((h) => h.id), ["b", "a", "d", "c"], JSON.stringify(fused));
  const bScore = 1 / 62 + 1 / 61;
  assert.ok(Math.abs(fused[0].meta.fusedScore - bScore) < 1e-9, JSON.stringify(fused[0]));
  assert.equal(fused[0].meta.engine, "A"); // richest/first-seen origin tag

  // k truncates.
  assert.equal(rcp.rrfFuse({ A, B }, { rrfK: 60, k: 2 }).length, 2);

  // Deterministic tie-break: equal fused score -> id ascending.
  const tie = rcp.rrfFuse({ X: [{ id: "z" }], Y: [{ id: "a" }] }, { rrfK: 60 });
  assert.deepEqual(tie.map((h) => h.id), ["a", "z"], JSON.stringify(tie));

  // Weighted fusion with per-engine min-max normalisation.
  const wf = rcp.weightedFuse(
    { A: [{ id: "a", score: 10 }, { id: "b", score: 0 }], B: [{ id: "b", score: 5 }] },
    { weights: { A: 1, B: 1 } },
  );
  // A: a->1.0, b->0.0 ; B: b->1.0 (single => norm 1). b=0+1=1 (weightSum 2), a=1
  // (weightSum 1). Fused equal -> §16.3 breaks on higher weight -> b before a.
  assert.deepEqual(wf.map((h) => h.id), ["b", "a"], JSON.stringify(wf));

  // Dedup keeps the richest body across engines.
  const dd = rcp.rrfFuse(
    { A: [{ id: "d", text: "short" }], B: [{ id: "d", text: "a much longer body" }] },
    { rrfK: 60 },
  );
  assert.equal(dd.length, 1);
  assert.equal(dd[0].text, "a much longer body", JSON.stringify(dd[0]));

  console.log("fusion (RRF + weighted): ok");
}

function testVectorCodec() {
  const vecs = [
    [1.5, -2.25, 0.0, 3.125],
    [0.5, 0.5, 0.5, 0.5],
  ];
  // json round-trips exactly.
  const j = rcp.encodeVectors(vecs, rcp.JSON_ENC);
  assert.deepEqual(rcp.decodeVectors(j.payload), vecs);

  // f32-base64 round-trips within float32 precision, carries dimension meta.
  const b = rcp.encodeVectors(vecs, rcp.F32_BASE64);
  assert.equal(b.meta.encoding, "f32-base64");
  assert.equal(b.meta.dimension, 4);
  const back = rcp.decodeVectors(b.payload, b.meta.encoding, b.meta.dimension);
  for (let i = 0; i < vecs.length; i++)
    for (let k = 0; k < 4; k++) assert.ok(Math.abs(back[i][k] - vecs[i][k]) < 1e-6);

  // Ragged vectors rejected under a binary encoding.
  assert.throws(() => rcp.encodeVectors([[1, 2], [3]], rcp.F32_BASE64));
  // Wrong declared dimension rejected on decode.
  assert.throws(() => rcp.decodeVectors(b.payload, "f32-base64", 8));

  console.log("f32-base64 vector codec: ok");
}

await testServerInproc();
await testClientAgainstCppServer();
await testRegistrySelector();
await testAgenticSurfaces();
await testFilter();
testFusion();
testVectorCodec();
console.log("\nall native Node.js SDK checks passed");
