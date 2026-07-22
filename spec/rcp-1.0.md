# Retrieval Context Protocol — Specification

**Version:** 1.0 (`RCP/1`)
**Status:** Stable
**Date:** 2026
**Editors:** RCP Working Group
**This document:** <https://1ay1.github.io/rcp/spec/rcp-1.0.html>
**Latest version:** <https://1ay1.github.io/rcp/spec/>
**Normative schema:** [`/schema/rcp-1.0.json`](../schema/rcp-1.0.json)

> The key words **MUST**, **MUST NOT**, **REQUIRED**, **SHALL**, **SHALL NOT**,
> **SHOULD**, **SHOULD NOT**, **RECOMMENDED**, **MAY**, and **OPTIONAL** in this
> document are to be interpreted as described in
> [RFC 2119](https://www.rfc-editor.org/rfc/rfc2119) and
> [RFC 8174](https://www.rfc-editor.org/rfc/rfc8174) when, and only when, they
> appear in all capitals, as shown here.

---

## Table of Contents

- [Abstract](#abstract)
- [1. Goals & Non-Goals](#1-goals--non-goals)
- [2. Roles](#2-roles)
- [3. The retrieval pipeline model](#3-the-retrieval-pipeline-model)
- [4. Message Format](#4-message-format) — requests, `_meta`, notifications, [data types](#46-data-types), [identifiers, concurrency & limits](#47-identifiers-concurrency--limits), [content & modality](#48-content--modality)
- [5. Transports](#5-transports) — [stdio](#51-stdio-recommended-default), [HTTP](#52-http), [compatibility envelope](#53-compatibility-envelope)
- [6. Capabilities](#6-capabilities) — [`retrieve` metadata](#61-retrieve-capability-metadata), [extension & registration policy](#62-extension--registration-policy)
- [7. Methods](#7-methods) — initialize, info, embed(×3), rerank, [retrieve](#77-retrieve--gated-by-retrieve-the-workhorse) + [determinism](#771-determinism--reproducibility), transform, graph, index(×2), catalog/list, cancel, ping, shutdown
- [8. Metadata filtering](#8-metadata-filtering)
- [9. Streaming & progress](#9-streaming--progress)
- [10. Pagination](#10-pagination)
- [11. Batching](#11-batching)
- [12. Errors](#12-errors) — [`error.data`](#121-errordata), [retryability](#122-retryability)
- [13. Session Lifecycle](#13-session-lifecycle)
- [14. Conformance](#14-conformance) — L0 / L1 / L2 levels
- [15. Security Considerations](#15-security-considerations) — trust boundaries, prompt injection, poisoning, DoS, privacy
- [16. Federation](#16-federation--querying-every-reachable-engine)
- [17. Observability](#17-observability) — log notifications, telemetry
- [Appendix A — Versioning Policy](#appendix-a--versioning-policy)
- [Appendix B — Mapping SOTA techniques to RCP](#appendix-b--mapping-sota-retrieval-to-rcp)
- [Appendix C — Relationship to MCP and ACP](#appendix-c--relationship-to-mcp-and-acp)
- [Appendix D — A worked session](#appendix-d--a-worked-session)
- [Appendix E — Change log](#appendix-e--change-log)
- [Appendix F — Evaluation & quality](#appendix-f--evaluation--quality)
- [Appendix G — Glossary](#appendix-g--glossary)
- [Appendix H — Normative references](#appendix-h--normative-references)

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
defines three: `notifications/progress` (§9), `notifications/cancel` (§7.14),
and `notifications/log` (§17.1). The `notifications/` prefix is **reserved** for
protocol notifications; a future revision may add more, and peers **MUST**
silently ignore any `notifications/*` method they do not recognise (it has no
`id`, so there is nothing to answer). Method names are otherwise `<verb>` or
`<verb>/<sub>` (e.g. `retrieve`, `index/add`); the `x-<vendor>/…` space is for
extensions (§6.2).

### 4.6 Data types

RCP messages use only standard JSON types; the following conventions are
normative:

- **Integers** (`k`, `dimension`, `topN`, vocabulary `indices`, byte offsets)
  are JSON numbers with no fractional part and **MUST** fit in a signed 53-bit
  range (IEEE-754 double-exact). Implementations **MUST NOT** emit them as
  strings.
- **Floats** (`score`, embedding components, `lambda`, `progress`) are JSON
  numbers. Special values (`NaN`, `Infinity`) are **NOT** valid JSON and **MUST
  NOT** be emitted; a server that would produce them **MUST** substitute a
  finite sentinel or omit the field.
- **Strings** are UTF-8. Servers **MUST** accept the full Unicode range in
  `query`/`text` and **MUST NOT** assume ASCII.
- **Booleans** are `true`/`false`, never `0`/`1` or `"true"`.
- **Absent vs. `null`.** An **absent** optional field means "unspecified; use the
  default." An explicit **`null`** is only meaningful where a schema lists
  `null` as a permitted type; elsewhere a peer **MUST** treat `null` as absent.
  Producers **SHOULD** omit optional fields rather than send `null`.
- **Enums** are lower-case strings exactly as written in this document
  (`"cross-encoder"`, `"hybrid"`); comparison is case-sensitive.
- **Timestamps & dates.** A point in time is either an **RFC 3339 / ISO 8601**
  string (`"2017-06-12T00:00:00Z"`, UTC **RECOMMENDED**) or an integer count of
  **milliseconds since the Unix epoch**. A `filter` field that holds a time
  **SHOULD** be declared with type `"date"` in the advertised `filter.fields`
  (§8); the server declares which of the two encodings it accepts and **MUST**
  compare consistently. `recency.field` (§7.7) references such a field.
- **Scores.** `Hit.score` and `rerank` scores are **server-defined** and only
  guaranteed **monotonic** (higher = more relevant) *within a single response
  from one server*. They are **not** normalised to a fixed range and **not**
  comparable across servers or across calls; `minScore` (§7.7) is interpreted in
  the server's own scale. Cross-engine merging **MUST** therefore use ranks
  (RRF, §16.3), not raw scores. A `trust.score` (§7.7) is the exception: it is
  normalised to `[0,1]`.
- **Unknown fields** in any object **MUST** be ignored by the receiver, never
  rejected (forward compatibility). This does not apply to `filter` *field
  names*, which are validated against the advertised set (§8).

### 4.7 Identifiers, concurrency & limits

**Request `id`.** The `id` **MUST** be unique among a client's in-flight requests
on a connection. A client **MAY** reuse an `id` value only after it has received
the response bearing that `id`. Servers **MUST** echo the request `id`
unchanged (same JSON type and value) in the response. `id` **MUST NOT** be
`null` on a request (a `null` id denotes a response to an unparseable request,
per JSON-RPC).

**Concurrency & ordering.** A client **MAY** have multiple requests in flight on
one connection (pipelining). A server **MAY** answer them in any order; clients
**MUST** correlate responses by `id`, not by arrival order. A server **MAY**
process requests concurrently. `initialize` is the exception: a client **MUST
NOT** send any other request until it has received the `initialize` response.

**Message size & limits.** RCP sets no hard message-size limit, but servers
**SHOULD** advertise and enforce operational bounds — `retrieve.maxK`,
`rerank.maxCandidates`, `embed.batchLimit` — and reject requests that exceed them
with `-32602` (`InvalidParams`), or shed load with `-32011` (`RateLimited`).
Clients **SHOULD** treat these advertised bounds as authoritative and pre-clamp.

### 4.8 Content & modality

Retrieval is no longer text-only. Visual-document models (ColPali, ColQwen) embed
rendered page images; audio and code have their own encoders. RCP models this
with a **Content block** — a tagged union used wherever a query, a passage, or an
indexed document body appears:

```json
{ "type": "text",  "text": "attention is all you need" }
{ "type": "image", "mimeType": "image/png", "data": "<base64>" }
{ "type": "image", "mimeType": "image/png", "uri": "https://…/page-3.png" }
{ "type": "blob",  "mimeType": "audio/wav", "uri": "file:///clip.wav" }
```

- `type` is `"text"`, `"image"`, `"audio"`, or `"blob"`. A block carries its
  payload inline as base64 `data` **or** by reference as `uri` (never both); a
  `text` block uses `text`. `mimeType` is **REQUIRED** for non-text blocks.
- A **modality** is the coarse kind of content: `"text"`, `"image"`, `"audio"`,
  `"code"`, or `"multimodal"`. A server advertises the modalities it accepts per
  capability (e.g. `embed.modalities`, `retrieve.modalities`); the default,
  when unadvertised, is `["text"]`. Modality is orthogonal to block `type`: code
  travels as a `"text"` block tagged with modality `"code"` (there is no
  `"code"` block type), and `"multimodal"` denotes a query or hit that mixes
  blocks of several types.
- **Backward compatibility.** Everywhere this spec accepts a `query` or
  `passages`/`texts` **string**, a server that advertises a non-text modality
  **MUST** also accept a Content block (or array of blocks) in the same field, and
  a text-only server **MAY** accept only the string form. A bare string is
  exactly equivalent to `{"type":"text","text":"…"}`. This keeps the common
  text path terse while making multimodality first-class.
- Servers **MUST** reject a block whose `type`/`mimeType` is outside the modalities
  they advertised with `-32005` (`OptionUnsupported`), and **MUST** treat a `uri`
  they will not or cannot fetch as `-32602` rather than silently ignoring it.

---

## 5. Transports

A message is byte-identical across transports; only framing differs.

### 5.1 stdio (RECOMMENDED default)

- The client launches the server as a subprocess and speaks JSON-RPC over the
  child's **stdin** (client→server) and **stdout** (server→client).
- **Framing** is newline-delimited JSON (NDJSON): each message is exactly one
  JSON object serialised with **no embedded newline** (`\n`, U+000A) and
  terminated by a single `\n`. Messages are UTF-8; a BOM **MUST NOT** be
  emitted. Because JSON string escaping renders any literal newline as `\n`, a
  compact `dump` is always single-line — no length-prefix framing is needed.
- A reader **MUST** tolerate blank lines between messages (ignore them) and
  **MUST NOT** assume messages arrive one per `read()`; buffer until `\n`.
- **stdout is protocol-only.** A server **MUST NOT** write anything but framed
  JSON-RPC to stdout. **stderr** is reserved for human-readable diagnostics and
  **MUST NOT** carry protocol messages; structured logs travel over
  `notifications/log` (§17), not stderr.
- EOF on stdin is an implicit `shutdown`: the server **SHOULD** finish in-flight
  requests, flush their responses, and exit.

### 5.2 HTTP

- Each request is `POST <base>/<method>` (default base `/rcp`) with the JSON-RPC
  object as the body and `Content-Type: application/json`. The method in the URL
  path **MUST** match the `method` in the body; on mismatch the server **MUST**
  return `-32600`.
- The response body is the JSON-RPC response with `Content-Type:
  application/json`.
- **HTTP status mapping.** The transport status is distinct from the JSON-RPC
  error. A well-formed JSON-RPC response — *including* one carrying a JSON-RPC
  `error` — **MUST** be returned with HTTP `200`. Non-`200` codes are reserved
  for transport-level failures the JSON-RPC layer never saw:

  | HTTP | When |
  |------|------|
  | `200` | Any well-formed JSON-RPC response (success **or** JSON-RPC error). |
  | `400` | Body is not valid JSON / not a JSON-RPC request (equivalent to `-32700`/`-32600` when a body cannot be produced). |
  | `401` / `403` | Deployment auth rejected the caller (RCP defines no auth; see below). |
  | `404` | Unknown base path (not an RCP endpoint). |
  | `429` | Transport-level rate limiting; **SHOULD** carry `Retry-After`. Mirrors `-32011`. |
  | `500` / `503` | Server/back-end failure that prevented producing a JSON-RPC response. Mirrors `-32603`/`-32010`. |

  Clients **SHOULD** prefer the JSON-RPC `error.code` when a `200` body is
  present and treat non-`200` codes as transport faults.
- **Streaming (SSE).** When the client sends `Accept: text/event-stream` and the
  server advertised `streaming`, the server **MAY** respond with
  `Content-Type: text/event-stream` and emit zero or more
  `notifications/progress` frames followed by exactly one final frame carrying
  the JSON-RPC response. Each frame is one SSE `data:` line whose payload is a
  compact JSON object, terminated by a blank line. The stream ends after the
  final response frame. If the client did not request SSE, the server **MUST**
  return a single buffered JSON response.
- **Sessions.** HTTP is request-scoped, so `initialize` state does not persist
  across connections by default. A stateless HTTP server **MAY** treat every
  request as implicitly initialized at its advertised capabilities and answer
  `initialize`/`info` idempotently; such a server **MUST** still reject
  capability-gated methods it does not advertise with `-32003`. A stateful HTTP
  server **MAY** issue a session token in `initialize.result._meta.session` and
  require it (e.g. header `RCP-Session`) on subsequent requests.
- **Authentication.** RCP carries no auth of its own. HTTP deployments **MAY**
  require standard mechanisms (`Authorization: Bearer …`, mTLS, API-key
  headers); these are opaque to the protocol and handled by the transport layer
  before dispatch. Servers **MUST** apply auth before `initialize`.
- **CORS.** A browser-facing server **SHOULD** set appropriate
  `Access-Control-Allow-*` headers and handle `OPTIONS` preflight; the RCP
  payload is unaffected.

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
| `embed` | `{ dimension:int, identity:str, batchLimit?:int, normalized?:bool, modalities?:[str] }` | Dense content→vector. |
| `sparseEmbed` | `{ identity:str, vocabulary?:int }` | Learned-sparse embedding (SPLADE): term→weight maps. |
| `multiVector` | `{ dimension:int, identity:str, similarity?:"dot"\|"cosine", modalities?:[str] }` | Multi-vector / late-interaction (ColBERT, ColPali) matrices. |
| `rerank` | `{ methods:[str], maxCandidates?:int, models?:[str] }` | Reranking; `methods` ⊆ `["cross-encoder","colbert","llm"]`. |
| `retrieve` | see §6.1 | Full query→hits retrieval. |
| `transform` | `{ methods:[str] }` | Query transformation; `methods` ⊆ `["rewrite","hyde","multi-query","decompose","step-back"]`. |
| `graph` | `{ ops:[str] }` | Graph retrieval; ops ⊆ `["local","global","drift","communities","neighbors"]`. |
| `index` | `{ writable:bool, chunking?:bool, contextual?:bool }` | Document add/delete; optional server-side chunking / contextual retrieval. |
| `filter` | `{ fields:[str]\|object, operators?:[str] }` | Metadata filtering support and the fields/operators allowed. |
| `streaming` | `{}` | Server may emit `notifications/progress` and incremental hits. |
| `pagination` | `{}` | List results support `cursor`/`nextCursor`. |
| `citations` | `{}` | Hits carry `citation`/`trust` fields. |
| `log` | `{ levels?:[str] }` | Server emits `notifications/log` (§17). |
| `catalog` | `{ engines:int }` | Server is an **aggregator/gateway**: it federates downstream RCP engines and answers `catalog/list` (see §7.13, §16). |

Each capability's value is an object (possibly `{}`); its **presence** is what
signals support. A peer **MUST** treat an unknown capability key, or an unknown
field inside a known capability, as informational and ignore it (§6.2).

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

### 6.2 Extension & registration policy

RCP is designed to grow additively without breaking the wire. Names are
partitioned into three spaces:

- **Core** — the method names, capability keys, enum values, and error codes
  defined in this document. These are reserved; only a future RCP revision may
  add to core (Appendix A).
- **Vendor extensions** — any implementer **MAY** introduce experimental or
  proprietary surface *without* central coordination by using the `x-<vendor>`
  convention: extension **methods** are named `x-<vendor>/<name>` (e.g.
  `x-acme/rerank-v2`), extension **capability keys** and **object fields** are
  named `x-<vendor>` or `x-<vendor>-<name>`. A peer **MUST** ignore any
  `x-`-prefixed name it does not understand. Extension methods are still gated:
  a client **MUST NOT** call `x-acme/*` unless the server advertised a matching
  `x-acme` capability.
- **`_meta`** — free-form side-band data (timings, trace ids, `progressToken`,
  session tokens) attached to any params or result object, never affecting core
  semantics. Keys are implementation-defined; unknown keys are ignored.

A name that proves broadly useful **SHOULD** be proposed for core registration in
a subsequent revision, at which point it drops its `x-` prefix. There is no
run-time registry service; capability negotiation *is* the discovery mechanism.

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

**Version negotiation.** The `protocolVersion` a peer puts on the wire is the
**highest** version it supports (each peer supports the contiguous range
`[1 .. protocolVersion]`). The negotiated version is
`min(client.protocolVersion, server.protocolVersion)` and is what the server
returns; it governs the connection for its whole lifetime. Because RCP/1 is the
floor, the result is always `≥ 1` in practice. The server **MUST** return
`-32002` (`VersionMismatch`) only if it cannot satisfy the floor (it supports no
version `≥ 1`). Conversely, a client that receives a negotiated version **lower**
than its own minimum supported version **MUST** abort the connection rather than
speak a dialect it does not implement.

**Client `capabilities`.** The client's `capabilities` object mirrors the
server's presence-⇒-supported shape (§6) and declares what the *client* can
consume. RCP/1 defines none as REQUIRED — a client that omits it, or sends `{}`,
simply relies on the negotiated defaults — but a client **MAY** advertise
`streaming` (it is prepared to receive `notifications/progress`) or `log` (it
will render `notifications/log`). A server **MUST** treat the client's
capabilities as hints and **MUST NOT** require any client capability to be
present; it **MUST NOT** emit a notification a client did not opt into via the
corresponding capability or per-request handle (`_meta.progressToken`,
`_meta.logLevel`, §17.1). Unknown client capability keys are ignored (§6.2).

### 7.2 `info` — REQUIRED

Same shape as `initialize.result`, no state change. Callable any time.

### 7.3 `embed` — gated by `embed`

**Params** `{ "inputs": [Content|string], "kind"?: "query"|"document" }`
**Result** `{ "vectors": [[float]] }`

`inputs` are Content blocks or bare strings (§4.8); a text-only server accepts
only strings, while a multimodal (e.g. CLIP-style) server that advertises
`embed.modalities` also accepts `image`/`audio` blocks and returns one vector per
input in the same order. `kind` lets asymmetric encoders (e5/BGE query vs
passage prefixes) select the right pooling. Requests **SHOULD** respect
`embed.batchLimit`. For backward compatibility a server **MUST** also accept the
legacy field name `texts` as a synonym for `inputs`.

### 7.4 `embed/sparse` — gated by `sparseEmbed`

**Params** `{ "texts": [string], "kind"?: "query"|"document" }`
**Result** `{ "sparse": [ { "indices": [int], "values": [float] } ] }`

Each entry is a sparse vector over the model vocabulary (SPLADE-style term
weights). `indices` are vocabulary ids; `values` are the learned weights.

### 7.5 `embed/multi` — gated by `multiVector`

**Params** `{ "inputs": [Content|string], "kind"?: "query"|"document" }`
**Result** `{ "matrices": [ [[float]] ], "dimension": int }`

Each input yields a **matrix** of token- (ColBERT) or patch- (ColPali/ColQwen)
level embeddings for **late-interaction** scoring. The relevance of a document
*D* to a query *Q* is the **MaxSim** sum: for each query vector, take its maximum
similarity against any document vector, then sum over query vectors:

```
MaxSim(Q, D) = Σ_{q ∈ Q}  max_{d ∈ D}  sim(q, d)          sim = dot or cosine
```

Clients score with MaxSim locally, or delegate via `rerank method:"colbert"`.
Servers **SHOULD** advertise `multiVector.similarity` (`"dot"` or `"cosine"`) and,
for text models, document tokenization so token offsets align with `chunk`
boundaries. Visual-document servers advertise `multiVector.modalities:["image"]`
and accept rendered pages as `image` Content blocks (§4.8); each page yields one
patch matrix.

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
{ "query": "string" | Content | [Content],
  "k": 10,                          // final result count
  "mode"?: "dense"|"sparse"|"hybrid",
  "modality"?: "text"|"image"|"audio"|"code"|"multimodal",  // default "text" (§4.8)
  "candidateK"?: 100,               // recall-stage candidate count before rerank
  "fusion"?: { "method": "rrf"|"weighted", "weights"?: {"dense":0.5,"sparse":0.5}, "rrfK"?: 60 },
  "rerank"?: { "method": "cross-encoder"|"colbert"|"llm", "model"?: "…", "topN"?: 10 } | false,
  "mmr"?: { "lambda": 0.5 },        // diversification (0=max diversity,1=pure relevance)
  "rewrite"?: "rewrite"|"hyde"|"multi-query"|"decompose"|false,
  "filter"?: { … },                 // metadata filter, see §8
  "minScore"?: 0.0,
  "recency"?: { "field": "timestamp", "halfLifeDays": 30 },
  "includeText"?: true,
  "includeVectors"?: false,
  "seed"?: 12345,                   // determinism hint (§7.7.1)
  "indexVersion"?: "opaque",        // pin retrieval to a corpus snapshot (§7.7.1)
  "strict"?: true,                  // reject vs. gracefully degrade unsupported options
  "cursor"?: "opaque",              // pagination (if `pagination` advertised)
  "_meta"?: { "progressToken": 1 }  // streaming (if `streaming` advertised)
}
```

**Result**
```json
{ "hits": [ Hit, … ],
  "nextCursor"?: "opaque",
  "indexVersion"?: "opaque",        // the snapshot actually served
  "usage"?: { "candidates": 100, "reranked": 30, "latencyMs": 42, "notes"?: ["…"] },
  "rewrittenQuery"?: "…" }
```

The server **MUST** honour options only within its capabilities and **SHOULD**
reflect what it actually did in `usage`. If a client requests an unsupported
`mode`/`method`, the server **MUST** return `-32005` (`OptionUnsupported`) rather
than silently degrade — unless the client set `"strict": false`, in which case
the server **MAY** fall back and note it in `usage.notes`. **`strict` defaults to
`true`** when absent.

**Stage-count invariant.** The three result-size knobs form a funnel and
**MUST** satisfy `candidateK ≥ rerank.topN ≥ k`: the recall stage produces
`candidateK` candidates (default server-chosen, e.g. `max(k, 100)`), the reranker
rescores the top `rerank.topN` of them, and the server returns the best `k`. A
server **MUST** clamp each value to `retrieve.maxK` / `rerank.maxCandidates` and
**SHOULD** report the effective counts in `usage` (`candidates`, `reranked`); it
**MUST NOT** return more than `k` hits (fewer is allowed when the corpus or
`minScore` yields fewer).

**Hit**
```json
{ "id": string | number,
  "score": float,
  "text"?: string,
  "content"?: [Content],              // non-text or mixed bodies (§4.8), e.g. a page image
  "modality"?: "text"|"image"|"audio"|"code"|"multimodal",
  "meta"?: object,
  "vector"?: [float],                 // when includeVectors
  "scores"?: { "dense": 0.7, "sparse": 0.3, "rerank": 0.9 },  // per-stage breakdown
  "citation"?: { "source": "…", "uri"?: "…", "title"?: "…", "start"?: int, "end"?: int, "page"?: int },
  "trust"?: { "level": "trusted"|"community"|"untrusted", "score"?: 0.0 },  // provenance (§15)
  "chunk"?: { "docId": string, "index": int, "context"?: string } }
```

`id` is in the server's identifier space; clients fusing across servers key on
the stringified `id`. `scores` exposes the per-stage contribution for
debugging/telemetry (SOTA pipelines want this). `trust` carries **provenance**
so a downstream LLM (or the client) can weight or quarantine untrusted content
before it enters a prompt (§15). `content`/`modality` carry non-text bodies for
visual-document and multimodal retrieval.

#### 7.7.1 Determinism & reproducibility

Retrieval quality work (evals, regression tests, A/Bs, audits) needs
repeatability. RCP provides two optional handles:

- **`seed`** — a client-supplied integer that fixes any stochastic step (ANN
  exploration, MMR tie-breaking, LLM-based rerank sampling). A server that
  honours `seed` **MUST** return identical results for identical
  `(query, params, seed, indexVersion)`; one that cannot **MUST** ignore it
  (never error) and **SHOULD** note non-determinism in `usage.notes`.
- **`indexVersion`** — an opaque token identifying a corpus snapshot. A server
  that supports snapshots echoes the served snapshot in the result and, when a
  client pins a specific `indexVersion`, **MUST** either serve that snapshot or
  fail with `-32010` (`BackendUnavailable`) if it has been garbage-collected —
  never silently serve a different one. This lets a client reproduce a result
  set even as the index changes underneath it.

A server advertises support via `retrieve.deterministic: true` and/or
`retrieve.snapshots: true`.

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

### 7.14 `notifications/cancel` — client notification

A client **MAY** ask a server to abandon an in-flight request (a long
`retrieve`, `rerank`, or `graph` call). Cancellation is a **notification** (no
`id`, no response):

```json
{ "jsonrpc": "2.0", "method": "notifications/cancel",
  "params": { "id": 7, "reason"?: "user aborted" } }
```

- `params.id` is the `id` of the request to cancel.
- On receipt the server **SHOULD** stop work for that request as soon as
  practical. It then **MUST** still send exactly one response for the cancelled
  request: either the normal result (if it had already completed or completion
  is cheaper than aborting) or an error with code `-32006` (`Cancelled`).
- Cancelling an unknown or already-answered `id` is a no-op the server **MUST**
  ignore. Because of the inherent race, a client **MUST** tolerate receiving a
  full successful result after it sent a cancel.
- Cancellation is always available; it needs no capability. It is a
  best-effort hint, not a guarantee of immediate termination.

### 7.15 `ping` — REQUIRED

Liveness / round-trip check. Callable any time (including before `initialize`),
never changes state.

**Params** `{ "nonce"?: string | number }` · **Result** `{ "nonce"?: same }`

The server **MUST** echo any `nonce` it received. Clients use `ping` for
keep-alive on long-lived connections and to measure round-trip latency; a
Selector (§16) uses it for the liveness probe behind primary→secondary fallback.

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

**Field types & value semantics.** A server **SHOULD** advertise `filter.fields`
as an object mapping field name → type, where type is one of `"keyword"`
(exact-match string), `"text"` (tokenised string), `"int"`, `"float"`, `"bool"`,
`"date"` (§4.6 timestamp), or `"geo"`. The `value` JSON type and the operators
allowed depend on the field type:

| Operator | `value` shape | Applies to | Meaning |
|----------|---------------|------------|---------|
| `eq`, `ne` | scalar (string / number / bool) | any | (in)equality |
| `gt`, `gte`, `lt`, `lte` | number or `date` | `int`/`float`/`date` | ordered comparison |
| `in`, `nin` | array of scalars | any | set membership / exclusion |
| `contains` | scalar | `text` (substring) or array-valued field (element membership) | containment |
| `exists` | boolean | any | field present (`true`) or absent (`false`) |

A `date` value follows §4.6 (RFC 3339 string or epoch-ms, as the server
declares). A server **MUST** return `-32602` (with `error.data.field`) if a
value's JSON type is incompatible with the field type or the operator is not
allowed for that field, and **MUST** reject an operator outside its advertised
`filter.operators` with `-32005`. Boolean combinators `and`/`or`/`not` nest
arbitrarily; an empty `and`/`or` array is `-32602`. Unless a server documents
otherwise, string comparison is case-sensitive and byte-ordered.

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

The `progressToken` is a client-chosen **string or integer**, unique among the
client's in-flight requests; the server **MUST** echo it verbatim in every
`notifications/progress` frame for that request and **MUST NOT** emit progress
for a request that carried none. A client **MUST** function correctly if it
never receives any progress frame (they are advisory), and **MUST** ignore a
frame bearing an unknown token.

## 10. Pagination

When `pagination` is advertised, list-style results (`retrieve`, `graph`
`communities`) **MAY** include an opaque `nextCursor`. To fetch the next page,
the client repeats the request with `cursor` set to that value. A missing/empty
`nextCursor` means no more results. Cursors are opaque and server-defined.

## 11. Batching

Clients **MAY** send a JSON array of request objects (JSON-RPC batch); the
server **MUST** reply with an array containing one response object per
*request* in the batch, in any order (clients correlate by `id`, §4.7).
Notifications in the batch (members with no `id`) produce **no** response entry.
Additional rules, per JSON-RPC 2.0:

- An **empty array** is an invalid request: the server **MUST** reply with a
  single (non-array) `-32600` error, `id: null`.
- If the batch contains **only notifications**, the server processes them and
  returns **nothing** (no HTTP body / no stdio line).
- A member that is not a well-formed request yields a `-32600` entry with
  `id: null`; well-formed members are still processed. One member's error never
  fails the others.
- Batching composes with capability gating and cancellation exactly as for
  singleton requests. Servers **MAY** cap batch size and reject an oversized
  batch with `-32602`.

`embed`, `embed/sparse`, `embed/multi`, and `rerank` are inherently batched via
their array params and **SHOULD** be preferred over JSON-RPC batching for
throughput.

---

## 12. Errors

Every failure is a JSON-RPC error object: `{ "code": int, "message": string,
"data"?: any }`. Codes in the `-32000..-32099` block are RCP-specific; the rest
are standard JSON-RPC.

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
| `-32006` | `Cancelled` | Request was cancelled via `notifications/cancel` (§7.14). |
| `-32010` | `BackendUnavailable` | Model offline, index not built, upstream down. |
| `-32011` | `RateLimited` | Server is shedding load; client should back off. |

The range `-32000..-32099` is reserved for RCP; implementers **MUST NOT** define
custom codes there. Vendor-specific failures **SHOULD** reuse the closest code
above and disambiguate in `error.data`.

### 12.1 `error.data`

Servers **MAY** attach a structured `error.data` object. When present it
**SHOULD** use these conventional fields (all optional):

| Field | Type | Meaning |
|-------|------|---------|
| `field` | string | The offending params path, e.g. `"filter.year"` (for `-32602`). |
| `option` | string | The unsupported mode/method (for `-32005`). |
| `retryable` | boolean | Whether an identical retry might succeed. |
| `retryAfterMs` | integer | Hint to wait before retrying (for `-32011`/`-32010`). |
| `detail` | string | Human-readable diagnostic (never for control flow). |

### 12.2 Retryability

Clients **SHOULD** apply the following retry policy. Retries **SHOULD** use
exponential backoff with jitter, honouring `retryAfterMs`/`Retry-After` when
given.

| Code | Retryable? | Client action |
|------|-----------|---------------|
| `-32011` `RateLimited` | yes | Back off and retry the same request. |
| `-32010` `BackendUnavailable` | yes (transient) | Retry with backoff; after a bound, fail over (Selector) or drop the engine (Federation). |
| `-32603` `InternalError` | maybe | Retry once; if it recurs, treat as fatal. |
| `-32001` `NotInitialized` | yes, after fixing | Send `initialize`, then retry. |
| `-32002` `VersionMismatch` | no | Abort; no common version. |
| `-32003` / `-32004` / `-32005` | no | Programming/capability error; do not retry. |
| `-32602` `InvalidParams` | no | Fix the request. |
| `-32006` `Cancelled` | n/a | Expected after a cancel; do not retry automatically. |

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

RCP defines three named conformance levels. A server **MUST** state its level in
`info`/`initialize` via `_meta.conformance` (`"L0"`, `"L1"`, or `"L2"`); clients
use it as a coarse expectation, capabilities as the precise contract.

### 14.1 Level L0 — Base (REQUIRED of every server)

An **L0** server **MUST**:

1. Answer `initialize` with negotiated `protocolVersion` ≥ 1, and answer `info`
   and `ping` at any time (including before `initialize`), echoing any `ping`
   `nonce`.
2. Advertise at least one retrieval capability (`embed`, `rerank`, `retrieve`,
   or `graph`).
3. Reject pre-`initialize` requests (other than `info`/`ping`) with `-32001`.
4. Reject un-advertised capability methods with `-32003`, undefined methods with
   `-32004`, and unsupported advertised-method options with `-32005`.
5. Return well-formed JSON-RPC 2.0 for every request; echo the request `id`
   unchanged (same type and value); validate params and return `-32602` on
   malformed input instead of crashing.
6. Ignore any notification it does not understand, and treat
   `notifications/cancel` for an unknown/already-answered `id` as a no-op.
7. Tolerate unknown object fields, unknown capability keys, and `_meta`
   (forward compatibility, §4.6/§6.2).

### 14.2 Level L1 — Retrieval (RECOMMENDED)

An **L1** server is L0 and additionally **SHOULD** implement `retrieve` with
`modes:["dense","sparse","hybrid"]`, support `filter` and `citations`, populate
`Hit.score` consistently (higher = more relevant), and honour `k`/`minScore`.
This is the target for a general-purpose RAG backend.

### 14.3 Level L2 — SOTA (OPTIONAL)

An **L2** server is L1 and additionally **SHOULD** advertise and implement a
selection of: `rerank` (cross-encoder and/or ColBERT), `multiVector` / late
interaction (incl. visual-document `modalities:["image"]`), `sparseEmbed`,
`query/transform`, `graph`, `streaming`, `pagination`, `log`, and reproducibility
(`retrieve.deterministic`/`snapshots`, §7.7.1). L2 is what lets a client drive a
full hybrid → fuse → rerank → MMR pipeline through one connection.

### 14.4 Client conformance

A **conforming client** **MUST** send `initialize` first, honour the negotiated
version, never call an unadvertised method or pass an unadvertised option (unless
`strict:false`), correlate responses by `id` (§4.7), tolerate unknown
fields/`_meta`/capability keys, and function correctly if it drops every
`notifications/progress` and `notifications/log` notification.

The JSON Schema in [`/schema`](../schema) is normative for message shapes; the
[`/conformance`](../conformance) suite validates any server, in any language,
over stdio or HTTP.

---

## 15. Security Considerations

RCP moves *untrusted external content* toward an LLM's context window, so its
threat model is dominated by two concerns absent from ordinary RPC: **indirect
prompt injection** (retrieved text is itself adversarial) and **provenance /
trust** of what gets grounded. Implementers **MUST** treat retrieved content as
untrusted data, never as instructions.

### 15.1 Trust boundaries

- **stdio** servers run with the launcher's privileges; a launched binary is
  *trusted code* — vet it as you would any dependency. The launcher **SHOULD**
  pass least-privilege environment and working directory.
- **HTTP** servers **SHOULD** bind to loopback unless fronted by authenticated,
  encrypted transport (TLS). RCP defines no auth of its own (§5.2); deployments
  layer `Authorization`, mTLS, or API keys at the transport and apply them
  *before* `initialize`.
- A **client MUST NOT** assume a server is honest about its `capabilities`: a
  malicious or buggy server can claim `citations` and fabricate `citation.uri`,
  or claim `trust.level:"trusted"` for poisoned content. Capability claims are
  hints for feature negotiation, not security guarantees.

### 15.2 Indirect prompt injection (the primary RAG threat)

Retrieved passages may contain text engineered to hijack a downstream LLM
(“ignore previous instructions…”). RCP cannot neutralise this alone, but it is
designed so a careful client can:

- Servers **SHOULD** populate `Hit.trust` with real provenance and **MUST NOT**
  label user-generated or web-scraped content `"trusted"`.
- Clients **SHOULD** keep retrieved content in a *data* channel distinct from
  the instruction channel of any prompt they build, delimit it unambiguously,
  and **SHOULD** down-weight or quarantine `trust.level:"untrusted"` hits.
- Servers and clients **MUST NOT** interpret any field of a `retrieve` result
  (text, `meta`, `citation`) as an RCP control instruction. There is no
  in-band mechanism by which retrieved content can change protocol behaviour.

### 15.3 Corpus / index poisoning

An attacker who can write to the index (via `index/add` or an upstream feed) can
plant documents crafted to rank highly for a target query and then inject or
mislead. Mitigations: servers **SHOULD** authenticate and authorise `index/add`
/ `index/delete` independently of read access; **SHOULD** record provenance so
tainted sources can be revoked; and **MAY** expose `indexVersion` (§7.7.1) so a
client can pin a vetted snapshot and audit what changed.

### 15.4 Injection into the backing query engine

`filter` trees, `graph` ops, and `id`s flow into a real query engine (SQL, a
vector DB filter DSL, a Cypher query). Servers **MUST** treat all `params` as
untrusted, validate `filter` field names against the advertised set (§8),
parameterise rather than string-concatenate, and reject unadvertised operators
with `-32602`. A server **MUST NOT** evaluate client-supplied expressions as
code.

### 15.5 Resource exhaustion & denial of service

- Servers **MUST** enforce `maxK` / `maxCandidates` / `batchLimit` and
  **SHOULD** cap total request body size, embedding batch size, `hops` on
  `graph`, and `candidateK`, returning `-32602` or shedding load with `-32011`.
- Servers **SHOULD** apply per-client rate limiting and per-request deadlines,
  and **SHOULD** bound the fan-out of `query/transform` (e.g. `multi-query n`).
- Clients **MUST** bound their own fan-out and set deadlines when federating
  (§16.2) so one slow engine cannot stall a query.
- Fetching a Content `uri` (§4.8) is an SSRF vector: a server **MUST** restrict
  which schemes/hosts it will dereference and **SHOULD** default to refusing
  `file://` and private-network URIs unless explicitly configured.

### 15.6 Data protection & privacy

- Queries and returned text may contain PII or secrets. Servers **SHOULD NOT**
  log full query/response bodies by default and **SHOULD** support redaction.
- Access control is a *server* responsibility: a multi-tenant server **MUST**
  scope retrieval to the caller's authorised corpus and **MUST NOT** leak one
  tenant's documents to another via shared `id` spaces or `catalog/list`.
- `catalog/list` and `info` reveal topology and capabilities; a server **MAY**
  restrict them to authenticated callers.

### 15.7 Supply chain & versioning

Pin server binaries and model identities (`embed.identity`); a silent model swap
changes the embedding space and corrupts a client-side ANN index. Treat
`indexVersion` and `embed.identity` as part of a reproducible retrieval
configuration.

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
is sorted by descending `RRF(d)` and truncated to `k`. The constant `rrfK`
(conventionally 60, per Cormack et al. 2009) damps the influence of low ranks;
larger values flatten the contribution curve.

**Weighted score fusion** is the alternative when engines expose *comparable*
scores. Because raw scores from different scorers are not comparable, they
**MUST** be normalised per engine first — min-max to `[0,1]` over that engine's
returned set is the RECOMMENDED normaliser:

```
norm_engine(d) = (score_engine(d) - min_engine) / (max_engine - min_engine)
Fused(d)       = Σ_engine  weight_engine · norm_engine(d)
```

RRF is preferred by default precisely because it sidesteps this normalisation
footgun. A client **MAY** also run a second-stage cross-encoder `rerank` over the
union of candidates — the most accurate option when one engine offers `rerank`.

**Tie-breaking.** Ties in the fused score **MUST** be broken deterministically so
fusion is reproducible: order by (1) higher fused score, then (2) higher summed
weight, then (3) lexicographically smaller stringified `id`. This guarantees a
total order independent of engine response arrival order.

**Deduplication.** Document identity across engines is the **stringified
`Hit.id`** (§7.7). Because `id` spaces differ between engines, a client **MAY**
instead key on a content hash or `citation.uri` when engines are known to share a
corpus; this is a client policy, not a protocol requirement. When two hits
collapse to one identity, the fused hit **SHOULD** retain the richest body
(longest `text` / most `content`) and merge `meta`.

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

## 17. Observability

Production retrieval needs to be debuggable and measurable. RCP exposes two
non-intrusive channels; both are optional and never alter retrieval semantics.

### 17.1 Log notifications

A server that advertises `log` **MAY** emit `notifications/log` (no `id`, no
response) at any time:

```json
{ "jsonrpc": "2.0", "method": "notifications/log",
  "params": { "level": "info", "message": "reranked 100→30",
              "logger"?: "pipeline", "data"?: { "latencyMs": 42 },
              "_meta"?: { "progressToken": "t2" } } }
```

- `level` is a syslog-style severity: `"debug"`, `"info"`, `"notice"`,
  `"warning"`, `"error"`. A client **MAY** advertise a minimum level in
  `initialize.params` (`_meta.logLevel`); servers **SHOULD** honour it and
  **SHOULD** emit logs only when the client advertised the `log` capability
  (§7.1).
- Logs are diagnostics only. A client **MUST** function correctly if it drops
  every `notifications/log`, and **MUST NOT** parse log text for control flow.
- Over stdio, logs travel as `notifications/log` on stdout — *not* on stderr —
  so a supervising client sees them in-band and correlated with requests.

### 17.2 Usage & telemetry

The `usage` object on a `retrieve`/`rerank`/`graph` result (§7.7) carries
per-request telemetry (`candidates`, `reranked`, `latencyMs`, `notes`). Servers
**SHOULD** populate it; clients **SHOULD** treat it as advisory. Trace
correlation ids belong in `_meta` (e.g. `_meta.traceId`), never in a core field.

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
| Visual-document retrieval (ColPali/ColQwen) | `multiVector modalities:["image"]`, `retrieve modality:"image"`, `image` Content blocks |
| Multimodal / cross-modal retrieval | `retrieve modality:"multimodal"`, Content blocks (§4.8) |
| Sparse late interaction (SPLATE) | `sparseEmbed` candidate-gen + `rerank method:"colbert"` |
| Matryoshka / truncatable embeddings | `embed.dimension` advertised; client truncates |
| MMR diversification | `retrieve.mmr` |
| Query rewrite / HyDE / multi-query | `query/transform` or `retrieve.rewrite` |
| Step-back prompting | `query/transform method:"step-back"` |
| Multi-hop / agentic | client loop over `query/transform` + `retrieve` + `graph:"drift"` |
| GraphRAG local/global | `graph op:"local"\|"global"` |
| Contextual retrieval (Anthropic) | `index/add contextual:true` |
| Citations / attribution | `citations` capability, `Hit.citation` |
| Provenance / trust weighting | `Hit.trust` (§15.2) |
| Freshness / recency bias | `retrieve.recency` |
| Metadata filtering | `filter` capability, `retrieve.filter` |
| Reproducible eval / regression tests | `retrieve.seed`, `retrieve.indexVersion` (§7.7.1), Appendix F |
| Multi-backend fan-out + fusion | Federation (§16), RRF / weighted (§16.3) |

## Appendix C — Relationship to MCP and ACP

An agent runtime speaks **ACP** to its client, calls tools via **MCP**, and
fetches grounding context via **RCP**. The three share JSON-RPC framing, the
`initialize`/capabilities handshake, `_meta` extensibility, and cursor-based
pagination, so an implementer of one is immediately productive in another.

## Appendix D — A worked session

A complete stdio session against a SOTA server, showing the handshake, a
hybrid+rerank+MMR retrieval with a filter and a progress stream, and shutdown.
`→` is client→server, `←` is server→client. One JSON object per line on the wire.

**1. Handshake.**

```json
→ {"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":1,"client":{"name":"my-app","version":"0.4"}}}
← {"jsonrpc":"2.0","id":1,"result":{"protocolVersion":1,"server":{"name":"acme-rag","version":"2.1"},"capabilities":{"retrieve":{"maxK":200,"modes":["dense","sparse","hybrid"],"citations":true},"rerank":{"methods":["cross-encoder","colbert"]},"filter":{"fields":{"year":"int","lang":"keyword"}},"streaming":{}}}}
```

**2. Retrieve** — hybrid recall, cross-encoder rerank of the top 100, MMR for
diversity, restricted to recent English docs, streamed.

```json
→ {"jsonrpc":"2.0","id":2,"method":"retrieve","params":{
     "query":"transformer attention complexity","k":5,"mode":"hybrid",
     "rerank":{"method":"cross-encoder","topN":100},
     "mmr":{"lambda":0.5},
     "filter":{"and":[{"field":"lang","op":"eq","value":"en"},{"field":"year","op":"gte","value":2017}]},
     "_meta":{"progressToken":"t2"}}}
← {"jsonrpc":"2.0","method":"notifications/progress","params":{"progressToken":"t2","progress":0.4,"message":"recall"}}
← {"jsonrpc":"2.0","method":"notifications/progress","params":{"progressToken":"t2","progress":0.8,"message":"rerank"}}
← {"jsonrpc":"2.0","id":2,"result":{"hits":[
     {"id":"arxiv:1706.03762#3","score":0.94,"text":"Attention is all you need …",
      "citation":{"uri":"https://arxiv.org/abs/1706.03762","title":"Attention Is All You Need"},
      "meta":{"year":2017}},
     {"id":"arxiv:2009.14794#1","score":0.81,"text":"Rethinking attention with Performers …"}
   ]}}
```

**3. Cancel a slow follow-up** (races with completion; client tolerates either
outcome).

```json
→ {"jsonrpc":"2.0","id":3,"method":"graph","params":{"op":"global","query":"survey of efficient attention"}}
→ {"jsonrpc":"2.0","method":"notifications/cancel","params":{"id":3,"reason":"user navigated away"}}
← {"jsonrpc":"2.0","id":3,"error":{"code":-32006,"message":"cancelled"}}
```

**4. Shutdown.**

```json
→ {"jsonrpc":"2.0","id":4,"method":"shutdown","params":{}}
← {"jsonrpc":"2.0","id":4,"result":{}}
```

## Appendix E — Change log

| Version | Date | Changes |
|---------|------|---------|
| `RCP/1` 1.0 | 2026 | Initial stable release. Core methods (`initialize`, `info`, `embed`, `embed/sparse`, `embed/multi`, `rerank`, `retrieve`, `query/transform`, `graph`, `index/add`, `index/delete`, `catalog/list`, `shutdown`, `notifications/cancel`, `ping`), capability negotiation, stdio + HTTP(+SSE) transports, a Content/modality model for multimodal & visual-document retrieval, metadata filtering, streaming/progress, `notifications/log` observability, pagination, batching, structured errors with retryability, determinism (`seed`/`indexVersion`), a full threat model, federation (registry + RRF/weighted fusion), and native C++, Python, Node.js, and Rust SDKs. |
| `RCP/1` 1.0 · ed. | 2026 | Clarifications and one notification rename. No changes to any request/response shape. Additions: normative timestamp/date encoding, score-scale & comparability rules, and `trust.score ∈ [0,1]` (§4.6); `filter` field-type × operator value-typing table and empty-combinator handling (§8); client `capabilities` semantics and tightened version-negotiation wording (§7.1); `embed` accepts Content blocks via `inputs`, with `texts` retained as a legacy synonym (§7.3); explicit `strict` default and the `candidateK ≥ rerank.topN ≥ k` funnel invariant (§7.7); `progressToken` typing/uniqueness (§9); JSON-RPC batch edge cases (§11). Rename: the log notification method `log` → `notifications/log`, and the `notifications/*` namespace is now reserved (§4.5, §17.1); the `log` *capability* key is unchanged. |

## Appendix F — Evaluation & quality

RCP is a transport, not a ranker, but it is designed so retrieval *quality* can
be measured and compared across engines. Because a `retrieve` result is a ranked
list of `Hit`s with stable `id`s, standard IR metrics apply directly. Given a
query with graded relevance judgements, common measures are:

- **Recall@k** — fraction of all relevant documents present in the top `k`.
- **MRR** — mean reciprocal rank of the first relevant hit: `mean(1/rank_first)`.
- **nDCG@k** — `DCG@k / IDCG@k`, `DCG@k = Σ_{i=1..k} rel_i / log2(i+1)`; rewards
  putting highly-relevant hits near the top.
- **MAP** — mean average precision across queries.

Reproducible evaluation relies on §7.7.1: fix `seed` and pin `indexVersion` so a
benchmark is repeatable, and read `usage.candidates`/`usage.reranked` to attribute
quality to each pipeline stage. A conformance-plus test harness **SHOULD** report
nDCG@10 and Recall@50 on a held-out set when comparing engines behind the same
RCP surface. Metric computation is intentionally client-side: any engine that
speaks RCP can be dropped into the same harness without change.

## Appendix G — Glossary

| Term | Meaning |
|------|---------|
| **Bi-encoder** | Encodes query and document independently into one vector each; fast, approximate (dense retrieval). |
| **Cross-encoder** | Jointly encodes a (query, document) pair for one relevance score; accurate, used for reranking. |
| **Late interaction** | Multi-vector scoring (MaxSim) that approximates cross-encoder accuracy at bi-encoder speed (ColBERT, ColPali). |
| **Learned-sparse** | A sparse term→weight vector produced by a model (SPLADE), searchable with an inverted index. |
| **Dense / sparse / hybrid** | Retrieval over dense vectors / lexical or learned-sparse terms / a fusion of both. |
| **RRF** | Reciprocal Rank Fusion — rank-only list merging robust to incomparable scores (§16.3). |
| **MMR** | Maximal Marginal Relevance — re-ranks for relevance–diversity trade-off. |
| **HyDE** | Hypothetical Document Embeddings — embed an LLM-generated pseudo-answer as the query. |
| **GraphRAG** | Retrieval over a knowledge graph / community summaries (local, global, drift). |
| **Contextual retrieval** | Prepending chunk-situating context before embedding/indexing (Anthropic). |
| **Recall stage / rerank stage** | Cheap high-recall candidate generation, then expensive precise reranking. |
| **Aggregator** | A server that federates downstream engines and advertises `catalog`. |
| **Modality** | Coarse content kind: text, image, audio, code, multimodal (§4.8). |

## Appendix H — Normative references

- **[RFC 2119]** Key words for use in RFCs to Indicate Requirement Levels.
- **[RFC 8174]** Ambiguity of Uppercase vs Lowercase in RFC 2119 Key Words.
- **[RFC 3986]** Uniform Resource Identifier (URI): Generic Syntax — for `uri` fields.
- **[RFC 4648]** The Base16, Base32, and Base64 Data Encodings — for inline `data`.
- **[RFC 8259]** The JavaScript Object Notation (JSON) Data Interchange Format.
- **[JSON-RPC 2.0]** JSON-RPC 2.0 Specification, <https://www.jsonrpc.org/specification>.
- **[JSON Schema 2020-12]** JSON Schema draft 2020-12 — the normative message schema at [`/schema/rcp-1.0.json`](../schema/rcp-1.0.json).

Informative: Cormack et al., *Reciprocal Rank Fusion* (SIGIR 2009); Khattab &
Zaharia, *ColBERT* (SIGIR 2020); Formal et al., *SPLADE* (2021) and *SPLATE*
(2024); Faysse et al., *ColPali* (2024).
