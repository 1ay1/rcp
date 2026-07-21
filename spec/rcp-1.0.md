# Retrieval Context Protocol — Specification

**Version:** 1.0 (`RCP/1`)
**Status:** Stable
**Date:** 2025
**Editors:** RCP Working Group

> The key words **MUST**, **MUST NOT**, **REQUIRED**, **SHALL**, **SHALL NOT**,
> **SHOULD**, **SHOULD NOT**, **RECOMMENDED**, **MAY**, and **OPTIONAL** in this
> document are to be interpreted as described in [RFC 2119](https://www.rfc-editor.org/rfc/rfc2119).

---

## Abstract

The **Retrieval Context Protocol (RCP)** is an open, transport-agnostic,
JSON-RPC 2.0 protocol that standardises how a *client* (an application, an agent
runtime, an IDE) obtains retrieval context from a *server* (a RAG engine, a
vector database, a search service, a knowledge graph).

RCP is to **retrieval** what the Model Context Protocol is to **tools** and the
Agent Client Protocol is to **agents**: a small, versioned contract so that any
retrieval engine — regardless of language, vendor, or internal architecture —
can expose its capabilities, and any client can consume them uniformly.

RCP is designed to express the **state of the art** in retrieval, not a lowest
common denominator. A single protocol surface covers:

- **Hybrid retrieval** — dense (bi-encoder), sparse lexical (BM25), and
  **learned-sparse** (SPLADE) matching, fused with RRF/weighted fusion.
- **Multi-stage reranking** — cross-encoder and **late-interaction (ColBERT
  MaxSim)** rerankers over a recall-oriented candidate set.
- **Multi-vector embeddings** — token-level matrices for late interaction, not
  just a single dense vector.
- **Query transformation** — rewrite, HyDE, multi-query, and decomposition for
  agentic / multi-hop retrieval.
- **GraphRAG** — local (entity-anchored), global (community-summary), and drift
  search over a knowledge graph.
- **Diversification (MMR)**, **metadata filtering**, **recency/freshness**,
  **contextual retrieval**, **citations/attribution**, and **grounding checks**.
- **Streaming** progress + incremental results, **pagination**, and **batching**.

A *minimal* conforming server implements one handshake method plus one retrieval
capability; every advanced feature above is **optional and negotiated**, so a
simple server and a SOTA engine speak the same protocol.

---

## 1. Goals & Non-Goals

### 1.1 Goals

- **Uniform access.** One client speaks to a local embedded index, a hosted
  vector DB, a Python research engine, and a graph store without bespoke glue.
- **Express the SOTA.** The protocol has first-class shapes for hybrid search,
  learned-sparse and multi-vector representations, reranking cascades, query
  transformation, and graph retrieval — the techniques production RAG actually
  uses (see the [pipeline in §3](#3-the-retrieval-pipeline-model)).
- **Capability negotiation.** Clients discover what a server can do at connect
  time and never call an unsupported method or pass an unsupported option.
- **Language & transport independence.** JSON-RPC 2.0 over newline stdio or
  HTTP. If it can read a line and parse JSON, it can be an RCP server.
- **Composability.** Servers chain, fuse, and re-export; a gateway can be a
  server to its clients and a client to upstream engines.
- **Forward compatibility.** A client built for `RCP/1` interoperates with any
  `RCP/1+` server using the intersection of their capabilities. Unknown fields
  and `_meta` extensions are always tolerated.

### 1.2 Non-Goals

- RCP does **not** define embedding model formats, index file formats, or the
  internal ranking math. Those are server implementation details.
- RCP does **not** define authentication beyond carrying opaque HTTP headers;
  deployment auth is layered on top.
- RCP does **not** mandate persistence, sharding, or consistency semantics.

---

## 2. Roles

| Role | Definition |
|------|------------|
| **Client** | Initiates the connection, sends `initialize`, and issues requests. |
| **Server** | Owns retrieval capability (an index, a model, a graph). |

A process **MAY** act as both (a gateway/router).

---

## 3. The retrieval pipeline model

RCP is shaped around the canonical production RAG pipeline, so each stage maps
to a protocol surface. A server implements whichever stages it offers:

```
query ─▶ [transform] ─▶ [retrieve: dense|sparse|hybrid] ─▶ [fuse]
      ─▶ [rerank: cross-encoder|colbert] ─▶ [diversify: mmr]
      ─▶ [pack + cite] ─▶ hits
```

| Stage | Method / option | Capability |
|-------|-----------------|------------|
| Query transformation | `query/transform`, or `retrieve.rewrite` | `transform` |
| Candidate retrieval | `retrieve` with `mode` = `dense`/`sparse`/`hybrid` | `retrieve` |
| Embedding (for client-side ANN) | `embed`, `embed/sparse`, `embed/multi` | `embed`, `sparseEmbed`, `multiVector` |
| Fusion | `retrieve.fusion` = `rrf`/`weighted` | (part of `retrieve.hybrid`) |
| Reranking | `rerank` (`method` = `cross-encoder`/`colbert`) | `rerank` |
| Diversification | `retrieve.mmr` | `retrieve.mmr` |
| Graph retrieval | `graph` (`op` = `local`/`global`/`drift`) | `graph` |
| Indexing | `index/add`, `index/delete` | `index` |

A client that only wants "the best hits for this query" calls `retrieve` and
lets the server run its full pipeline. A client that orchestrates its own
pipeline can call the stages individually.

---

## 4. Message Format

All RCP messages are [JSON-RPC 2.0](https://www.jsonrpc.org/specification)
objects. The `jsonrpc` member **MUST** be `"2.0"`.

### 4.1 Request

```json
{ "jsonrpc": "2.0", "id": 1, "method": "retrieve",
  "params": { "query": "…", "k": 5, "_meta": {} } }
```

- `id` **MUST** be a string or number, unique for the connection until answered.
- `method` **MUST** be a method name from §7.
- `params` **MUST** be an object (RCP does not use positional parameters).

### 4.2 Response — success

```json
{ "jsonrpc": "2.0", "id": 1, "result": { "hits": [ … ] } }
```

### 4.3 Response — error

```json
{ "jsonrpc": "2.0", "id": 1, "error": { "code": -32004, "message": "…" } }
```

### 4.4 `_meta` and extensions

Every params and result object **MAY** carry a `_meta` object for
implementation-specific data (timings, trace ids, a `progressToken`). Peers
**MUST** ignore `_meta` keys they do not understand. Vendor extension methods
use a `x-<vendor>/` prefix; vendor capability/field keys use a `x-<vendor>`
prefix. This keeps the core namespace clean and forward-compatible.

### 4.5 Notifications

A message with no `id` is a notification and **MUST NOT** be answered. RCP/1
defines `notifications/progress` (§9), emitted by a server during a long request
only if the client attached a `progressToken` and the server advertised
`streaming`.

---

## 5. Transports

A message is byte-identical across transports; only framing differs.

### 5.1 stdio (RECOMMENDED default)

- The client launches the server as a subprocess.
- Each JSON-RPC message is one compact (newline-free) JSON object terminated by
  `\n`, written to the server's **stdin**; responses go to its **stdout**.
- **stderr** is reserved for human-readable logging; it **MUST NOT** carry
  protocol messages.
- EOF on stdin is an implicit `shutdown`.

### 5.2 HTTP

- Each request is `POST <base>/<method>` (default base `/rcp`) with the JSON-RPC
  object as the body and `Content-Type: application/json`.
- The response body is the JSON-RPC response with HTTP `200` for any well-formed
  JSON-RPC response (including JSON-RPC-level errors).
- Server-Sent Events (`Accept: text/event-stream`) **MAY** be used to deliver
  `notifications/progress` frames followed by the final response, when the
  server advertises `streaming`.

### 5.3 Compatibility envelope

A server **MAY** also accept the compact `{ "method", "params" }` envelope.
Clients **MUST** send full JSON-RPC 2.0.

---

## 6. Capabilities

Capabilities follow **presence ⇒ supported**. A capability object's *presence*
means the method/feature is offered; its *value* carries metadata (which **MAY**
be `{}`). Absence means unavailable; clients **MUST NOT** use it.

| Key | Value shape | Meaning |
|-----|-------------|---------|
| `embed` | `{ dimension:int, identity:str, batchLimit?:int, normalized?:bool }` | Dense text→vector. |
| `sparseEmbed` | `{ identity:str, vocabulary?:int }` | Learned-sparse embedding (SPLADE): term→weight maps. |
| `multiVector` | `{ dimension:int, identity:str }` | Multi-vector / late-interaction (ColBERT) token matrices. |
| `rerank` | `{ methods:[str], maxCandidates?:int }` | Reranking; `methods` ⊆ `["cross-encoder","colbert","llm"]`. |
| `retrieve` | see §6.1 | Full query→hits retrieval. |
| `transform` | `{ methods:[str] }` | Query transformation; `methods` ⊆ `["rewrite","hyde","multi-query","decompose","step-back"]`. |
| `graph` | `{ ops:[str] }` | Graph retrieval; ops ⊆ `["local","global","drift","communities","neighbors"]`. |
| `index` | `{ writable:bool, chunking?:bool, contextual?:bool }` | Document add/delete; optional server-side chunking / contextual retrieval. |
| `filter` | `{ fields:[str], operators?:[str] }` | Metadata filtering support and the fields/operators allowed. |
| `streaming` | `true` | Server may emit `notifications/progress` and incremental hits. |
| `pagination` | `true` | List results support `cursor`/`nextCursor`. |
| `citations` | `true` | Hits carry `citation`/`attribution` fields. |
| `catalog` | `{ engines:int }` | Server is an **aggregator/gateway**: it federates downstream RCP engines and answers `catalog/list` (see §7.13, §16). |

### 6.1 `retrieve` capability metadata

```json
{ "retrieve": {
    "maxK": 200,
    "modes": ["dense", "sparse", "hybrid"],
    "fusion": ["rrf", "weighted"],
    "mmr": true,
    "rerankBuiltin": true,
    "defaultMode": "hybrid"
} }
```

- `modes` — the retrieval modes the server supports.
- `fusion` — fusion strategies available when `mode:"hybrid"`.
- `mmr` — whether MMR diversification can be requested inline.
- `rerankBuiltin` — whether the server can rerank inside `retrieve` (vs a
  separate `rerank` call).

Servers **MAY** add extension capability keys prefixed `x-<vendor>`; clients
**MUST** ignore unrecognised keys.

---

## 7. Methods

All methods after `initialize` are gated on the matching capability.

### 7.1 `initialize` — REQUIRED

The first request. No other method except `info` may precede it.

**Params**
```json
{ "protocolVersion": 1,
  "client": { "name": "my-client", "version": "1.0" },
  "capabilities": {} }
```
**Result**
```json
{ "protocolVersion": 1,
  "server": { "name": "my-engine", "version": "2.3.1" },
  "capabilities": { "retrieve": { "maxK": 200, "modes": ["dense","sparse","hybrid"],
                                  "fusion": ["rrf"], "mmr": true },
                    "rerank": { "methods": ["cross-encoder","colbert"] },
                    "embed": { "dimension": 384, "identity": "bge-small-en" } } }
```

**Version negotiation.** Each peer supports `[1 .. maxVersion]`; the negotiated
version is `min(client.protocolVersion, server.maxVersion)`. If `< 1`, the
server **MUST** return `-32002`.

### 7.2 `info` — REQUIRED

Same shape as `initialize.result`, no state change. Callable any time.

### 7.3 `embed` — gated by `embed`

**Params** `{ "texts": [string], "kind"?: "query"|"document" }`
**Result** `{ "vectors": [[float]] }`

`kind` lets asymmetric encoders (e5/BGE query vs passage prefixes) select the
right pooling. Requests **SHOULD** respect `embed.batchLimit`.

### 7.4 `embed/sparse` — gated by `sparseEmbed`

**Params** `{ "texts": [string], "kind"?: "query"|"document" }`
**Result** `{ "sparse": [ { "indices": [int], "values": [float] } ] }`

Each entry is a sparse vector over the model vocabulary (SPLADE-style term
weights). `indices` are vocabulary ids; `values` are the learned weights.

### 7.5 `embed/multi` — gated by `multiVector`

**Params** `{ "texts": [string], "kind"?: "query"|"document" }`
**Result** `{ "matrices": [ [[float]] ] }`

Each text yields a **matrix** of token embeddings (ColBERT). Clients score with
MaxSim; servers **SHOULD** document tokenization so offsets align.

### 7.6 `rerank` — gated by `rerank`

**Params**
```json
{ "query": "…",
  "passages": ["…", "…"],
  "method"?: "cross-encoder" | "colbert" | "llm",
  "topN"?: 10 }
```
**Result** `{ "scores": [float], "order"?: [int] }`

`scores[i]` corresponds to `passages[i]`; higher is more relevant. Optional
`order` gives the indices sorted best-first (convenient when `topN` truncates).
`method` **MUST** be in `rerank.methods`.

### 7.7 `retrieve` — gated by `retrieve`  *(the workhorse)*

Runs the server's retrieval pipeline and returns ranked hits. Every option
beyond `query`/`k` is optional and **MUST** be ignored-or-rejected only per the
advertised `retrieve` capability.

**Params**
```json
{ "query": "string",
  "k": 10,                          // final result count
  "mode"?: "dense"|"sparse"|"hybrid",
  "candidateK"?: 100,               // recall-stage candidate count before rerank
  "fusion"?: { "method": "rrf"|"weighted", "weights"?: {"dense":0.5,"sparse":0.5}, "rrfK"?: 60 },
  "rerank"?: { "method": "cross-encoder"|"colbert"|"llm", "topN"?: 10 } | false,
  "mmr"?: { "lambda": 0.5 },        // diversification (0=max diversity,1=pure relevance)
  "rewrite"?: "rewrite"|"hyde"|"multi-query"|"decompose"|false,
  "filter"?: { … },                 // metadata filter, see §8
  "minScore"?: 0.0,
  "recency"?: { "field": "timestamp", "halfLifeDays": 30 },
  "includeText"?: true,
  "includeVectors"?: false,
  "cursor"?: "opaque",              // pagination (if `pagination` advertised)
  "_meta"?: { "progressToken": 1 }  // streaming (if `streaming` advertised)
}
```

**Result**
```json
{ "hits": [ Hit, … ],
  "nextCursor"?: "opaque",
  "usage"?: { "candidates": 100, "reranked": 30, "latencyMs": 42 },
  "rewrittenQuery"?: "…" }
```

The server **MUST** honour options only within its capabilities and **SHOULD**
reflect what it actually did in `usage`. If a client requests an unsupported
`mode`/`method`, the server **MUST** return `-32005` (`OptionUnsupported`) rather
than silently degrade — unless the client set `"strict": false`, in which case
the server **MAY** fall back and note it in `usage.notes`.

**Hit**
```json
{ "id": string | number,
  "score": float,
  "text"?: string,
  "meta"?: object,
  "vector"?: [float],                 // when includeVectors
  "scores"?: { "dense": 0.7, "sparse": 0.3, "rerank": 0.9 },  // per-stage breakdown
  "citation"?: { "source": "…", "uri"?: "…", "start"?: int, "end"?: int },
  "chunk"?: { "docId": string, "index": int, "context"?: string } }
```

`id` is in the server's identifier space; clients fusing across servers key on
the stringified `id`. `scores` exposes the per-stage contribution for
debugging/telemetry (SOTA pipelines want this).

### 7.8 `query/transform` — gated by `transform`

Standalone query transformation for clients driving an agentic loop.

**Params** `{ "query": "…", "method": "rewrite"|"hyde"|"multi-query"|"decompose"|"step-back", "n"?: 3 }`
**Result** `{ "queries": ["…", …], "hypothetical"?: "…" }`

`multi-query`/`decompose` return several queries; `hyde` returns a hypothetical
document in `hypothetical` (which the client then embeds).

### 7.9 `graph` — gated by `graph`

**Params** `{ "op": "local"|"global"|"drift"|"communities"|"neighbors", "query"?: "…", "k"?: 10, "seedIds"?: [string], "hops"?: int }`
**Result** — op-specific:

- `local` → `{ "hits": [Hit] }` (entity-anchored subgraph, PPR-expanded).
- `global` → `{ "summary": "…", "communities": [ { "id", "summary", "score" } ] }`.
- `drift` → `{ "hits": [Hit], "followups": ["…"] }` (dynamic reasoning).
- `communities` → `{ "communities": [ { "id", "level", "summary" } ] }`.
- `neighbors` → `{ "nodes": [ { "id", "score" } ], "edges": [ { "src","dst","weight" } ] }`.

`op` **MUST** be in `graph.ops`.

### 7.10 `index/add` — gated by `index` (`writable:true`)

**Params**
```json
{ "documents": [ { "id"?: string, "text": string, "meta"?: object } ],
  "chunk"?: bool,          // request server-side chunking (needs index.chunking)
  "contextual"?: bool }    // add situating context per chunk (needs index.contextual)
```
**Result** `{ "ids": [string], "chunks"?: int }`

### 7.11 `index/delete` — gated by `index`

**Params** `{ "ids": [string] }` · **Result** `{ "deleted": int }`

### 7.12 `shutdown` — REQUIRED

**Params** `{}` · **Result** `{}`. EOF on stdin is an equivalent shutdown.

### 7.13 `catalog/list` — gated by `catalog`

Returns the downstream engines an aggregator federates, so a client can
discover many engines through one endpoint (dynamic discovery, complementing the
static registry of §16).

**Params** `{}`
**Result**
```json
{ "engines": [
    { "id": "docs",
      "server": { "name": "docs-engine", "version": "1.0" },
      "capabilities": { "retrieve": { "maxK": 100 } },
      "endpoint"?: { "transport": "http", "url": "http://…/rcp" } } ] }
```

Each entry mirrors an `initialize.result` (identity + capabilities) plus an
optional `endpoint` a client may connect to directly. An aggregator that only
proxies (never exposes downstream endpoints) omits `endpoint`; clients then
query through the aggregator's own `retrieve`, which fuses internally.

---

## 8. Metadata filtering

When `filter` is advertised, `retrieve.filter` accepts a small boolean tree.
Servers advertise allowed `fields` and `operators`.

```json
{ "and": [
    { "field": "lang", "op": "eq", "value": "en" },
    { "or": [
        { "field": "year", "op": "gte", "value": 2023 },
        { "field": "tag", "op": "in", "value": ["news", "blog"] } ] } ] }
```

Operators: `eq`, `ne`, `gt`, `gte`, `lt`, `lte`, `in`, `nin`, `contains`,
`exists`. Servers **MUST** reject filters referencing unadvertised fields with
`-32602`.

---

## 9. Streaming & progress

When the client attaches `_meta.progressToken` and the server advertised
`streaming`, the server **MAY** send, before the final response:

```json
{ "jsonrpc": "2.0", "method": "notifications/progress",
  "params": { "progressToken": 1, "progress": 0.6, "stage": "rerank",
              "message": "reranked 30/50", "partial"?: { "hits": [ … ] } } }
```

`progress` is `0.0–1.0`. `stage` is one of the pipeline stages. `partial.hits`
**MAY** carry incremental results (e.g. first-stage candidates before rerank).
The final JSON-RPC response with the request `id` still follows and is
authoritative.

## 10. Pagination

When `pagination` is advertised, list-style results (`retrieve`, `graph`
`communities`) **MAY** include an opaque `nextCursor`. To fetch the next page,
the client repeats the request with `cursor` set to that value. A missing/empty
`nextCursor` means no more results. Cursors are opaque and server-defined.

## 11. Batching

Clients **MAY** send a JSON array of request objects (JSON-RPC batch); the
server **MUST** reply with an array of the corresponding responses. `embed`,
`embed/sparse`, `embed/multi`, and `rerank` are inherently batched via their
array params and **SHOULD** be preferred over JSON-RPC batching for throughput.

---

## 12. Errors

| Code | Symbol | Meaning |
|------|--------|---------|
| `-32700` | `ParseError` | Invalid JSON. |
| `-32600` | `InvalidRequest` | Not a valid Request object. |
| `-32601` | `MethodNotFound` | (Reserved; RCP prefers `-32004`.) |
| `-32602` | `InvalidParams` | Missing/malformed params or filter field. |
| `-32603` | `InternalError` | Server-side failure. |
| `-32001` | `NotInitialized` | Called before `initialize`. |
| `-32002` | `VersionMismatch` | No common protocol version. |
| `-32003` | `CapabilityMissing` | Method exists but not advertised. |
| `-32004` | `UnknownMethod` | Method not defined by RCP. |
| `-32005` | `OptionUnsupported` | A requested option (mode/method) is not advertised. |
| `-32010` | `BackendUnavailable` | Model offline, index not built, upstream down. |
| `-32011` | `RateLimited` | Server is shedding load; client should back off. |

Servers **MAY** attach `error.data`.

---

## 13. Session Lifecycle

```
Client                              Server
  │  ── initialize ─────────────────▶ │  negotiate version, cache caps
  │  ◀──────────────── result ─────── │
  │  ── query/transform (optional) ─▶ │  agentic query planning
  │  ── retrieve (hybrid+rerank+mmr) ▶ │  full pipeline
  │  ◀── notifications/progress ×N ── │  (if streaming)
  │  ◀──────────────── result ─────── │
  │  ── shutdown ───────────────────▶ │
```

A server **MUST** reject any non-`initialize`/`info` request before a successful
`initialize` with `-32001`.

---

## 14. Conformance

A **minimal RCP/1 server** **MUST**:

1. Answer `initialize` with negotiated `protocolVersion` ≥ 1 and `info` any time.
2. Advertise at least one of `embed`, `rerank`, `retrieve`, `graph`.
3. Reject pre-`initialize` requests with `-32001`.
4. Reject un-advertised capability methods with `-32003`, undefined methods with
   `-32004`, and unsupported advertised-method options with `-32005`.
5. Return well-formed JSON-RPC 2.0 for every request; validate params and return
   `-32602` on malformed input instead of crashing.

A **SOTA RCP/1 server** additionally **SHOULD** advertise and implement
`retrieve` with `modes:["dense","sparse","hybrid"]`, `rerank` with a
cross-encoder and/or ColBERT method, `filter`, and `citations`, and **MAY**
implement `transform`, `graph`, `multiVector`, `sparseEmbed`, `streaming`, and
`pagination`.

A **conforming client** **MUST** send `initialize` first, honour the negotiated
version, never call an unadvertised method or pass an unadvertised option
(unless `strict:false`), and tolerate unknown fields/`_meta`/capability keys.

The JSON Schemas in [`/schema`](../schema) are normative for message shapes; the
[`/conformance`](../conformance) suite validates any server.

---

## 15. Security Considerations

- **stdio** servers run with the launcher's privileges; treat server binaries as
  trusted code.
- **HTTP** servers **SHOULD** bind to loopback unless behind authenticated
  transport; RCP carries no auth of its own.
- Servers **MUST** treat all `params` as untrusted, validate before use, and
  sanitise `filter` trees and `graph` ops that reach an underlying query engine
  to prevent injection.
- Servers **SHOULD** enforce `maxK`/`maxCandidates`/`batchLimit` and return
  `-32011` under load rather than exhausting resources.

---

## 16. Federation — querying every reachable engine

RCP is point-to-point: one server answers for its own index. Querying **all**
the engines you can reach is a *client* concern, layered on top of the core
methods with no new server obligations. It has three parts: **discovery**,
**fan-out**, and **fusion**.

### 16.1 Discovery

A client learns which engines exist from either (or both):

1. **A static registry** — a config document listing reachable servers. The
   canonical shape (file name `rcp.json`, or a `[rcp]` table in a host's own
   config) is:

   ```json
   { "engines": [
       { "id": "docs",  "transport": "stdio", "command": ["python3", "docs_server.py"] },
       { "id": "web",   "transport": "http",  "url": "http://127.0.0.1:8000/rcp" },
       { "id": "code",  "transport": "stdio", "command": ["rcp-code-engine"],
         "weight": 2.0, "required": false } ] }
   ```

   `id` is a client-local label used to tag fused hits by origin. `weight`
   (default `1.0`) scales an engine's contribution during fusion. `required`
   (default `false`) — if `true`, a connect/query failure of that engine fails
   the whole federated query instead of being skipped.

2. **Dynamic discovery via an aggregator** — connect to one server that
   advertises the `catalog` capability and call `catalog/list` (§7.13) to learn
   the engines it federates, connecting to any it exposes an `endpoint` for.

The two compose: a registry entry may itself point at an aggregator.

### 16.2 Fan-out

Given N connected engines, a federated `retrieve(query, k)` issues `retrieve` to
each engine that advertises the `retrieve` capability, concurrently. Engines
lacking a requested capability are skipped (not an error). A slow or failing
non-`required` engine is dropped after a client-side deadline so one bad engine
cannot stall the query. Each engine is asked for at least `k` hits (a client
**SHOULD** request `k` or a small multiple to give fusion headroom).

### 16.3 Fusion

The client merges the per-engine ranked lists into one. The **default and
RECOMMENDED** strategy is **Reciprocal Rank Fusion (RRF)**, which needs only
ranks (not comparable scores) and so is robust across heterogeneous engines:

```
RRF(d) = Σ_engine  weight_engine / (rrfK + rank_engine(d))          rrfK default 60
```

where `rank_engine(d)` is the 1-based position of document `d` in that engine's
result list (engines that did not return `d` contribute nothing). The fused list
is sorted by descending `RRF(d)` and truncated to `k`.

Document identity across engines is the **stringified `Hit.id`** (§7.7). Because
`id` spaces differ between engines, a client **MAY** instead key on a content
hash or `citation.uri` when engines are known to share a corpus; this is a
client policy, not a protocol requirement. Alternatives to RRF (weighted score
normalisation, a second-stage cross-encoder `rerank` over the union) are
permitted; RRF is the interoperable default.

Each fused hit **SHOULD** carry its origin in `meta.engine` (the registry `id`)
and **MAY** retain `meta.engineRank`/`meta.engineScore` for debugging.

### 16.4 Federated capabilities

A client's *effective* capability set over a federation is the **union** of its
engines' capabilities (it can `embed` if any engine can). Its *guaranteed* set
is the **intersection**. A federated `retrieve` runs against the subset of
engines advertising `retrieve`; a federated `graph` op runs against those
advertising `graph` with that op. Reference SDKs expose both a per-engine handle
and a `Federation` facade implementing this section.

---

## Appendix A — Versioning Policy

- The major version (`RCP/N`) increments only on a breaking wire change.
- Additive changes (new optional methods, capability keys, fields, `_meta`) are
  made within a major version and discovered via capabilities.
- The negotiated version governs a session for its whole lifetime.

## Appendix B — Mapping SOTA techniques to RCP

| Technique | RCP surface |
|-----------|-------------|
| BM25 / lexical | `retrieve mode:"sparse"` |
| Dense bi-encoder (e5/BGE/GTE) | `retrieve mode:"dense"`, or `embed` for client-side ANN |
| Learned-sparse (SPLADE) | `sparseEmbed` + `retrieve mode:"sparse"` |
| Hybrid + RRF | `retrieve mode:"hybrid", fusion:{method:"rrf"}` |
| Cross-encoder rerank | `rerank method:"cross-encoder"` or `retrieve.rerank` |
| Late interaction (ColBERT) | `multiVector` + `rerank method:"colbert"` |
| MMR diversification | `retrieve.mmr` |
| Query rewrite / HyDE / multi-query | `query/transform` or `retrieve.rewrite` |
| Multi-hop / agentic | client loop over `query/transform` + `retrieve` + `graph:"drift"` |
| GraphRAG local/global | `graph op:"local"|"global"` |
| Contextual retrieval (Anthropic) | `index/add contextual:true` |
| Citations / attribution | `citations` capability, `Hit.citation` |
| Freshness / recency bias | `retrieve.recency` |
| Metadata filtering | `filter` capability, `retrieve.filter` |

## Appendix C — Relationship to MCP and ACP

An agent runtime speaks **ACP** to its client, calls tools via **MCP**, and
fetches grounding context via **RCP**. The three share JSON-RPC framing, the
`initialize`/capabilities handshake, `_meta` extensibility, and cursor-based
pagination, so an implementer of one is immediately productive in another.
