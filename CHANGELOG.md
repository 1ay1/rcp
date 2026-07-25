# Changelog

All notable changes to the Retrieval Context Protocol are documented here.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

RCP/1 is **stable**. Changes within the major version are additive and
capability-discovered — the wire never breaks under you.

The authoritative change log is
[Appendix E of the specification](spec/rcp-1.0.md#appendix-e--change-log); the
rendered version lives at the
[Changelog page](https://rcp-6d6ef6d5.mintlify.site/reference/changelog).

## [1.0 · ed.3] — 2026 (adoption & developer-experience revision)

Makes conformance *executable and certifiable*, and turns the reference server
and SDKs into a working demonstration of the SOTA the spec describes. Additive;
no wire change.

### Added
- **Index-write conformance checks** (§7.10/§7.11). `conformance/check.py` now
  verifies, for any server advertising `index.writable`, the normative write
  MUSTs: `index/add` returns positional `ids` (one per input document),
  re-adding an explicit `id` **upserts** (or rejects with `-32016`) rather than
  duplicating, and `index/delete` is idempotent (a repeat returns `deleted:0`).
  Gated on the capability, so read-only servers skip cleanly. This closes the
  gap where a whole normative section had no executable check behind it; first
  exercised by the rag-cpp (`github.com/…/rag-cpp`) RCP server, which certifies
  L2 with these enabled.
- **Level-aware certification harness** (§14.5). `conformance/check.py` now tags
  every check L0/L1/L2, reports the highest level a server actually reaches
  (`CERTIFIED LEVEL: …`), skips checks for unadvertised capabilities, and
  cross-checks the server's self-declared `_meta.conformance` — a server that
  **overclaims** fails even with zero failed checks. `--json` emits a
  CI-consumable report; non-zero exit gates a release pipeline.
- **Reference RRF / weighted fusion** in the Python SDK (`rcp.rrf_fuse`,
  `rcp.weighted_fuse`) — the federation algorithm from §16.3 as importable,
  tested code (deterministic tie-break, richest-body dedup, origin tagging) so
  no adopter re-derives it. Verified against a hand computation.
- `Server.set_conformance("L0"|"L1"|"L2")` in the Python SDK, surfaced as
  `_meta.conformance` in `initialize`/`info` (§14).
- **Cross-SDK fusion + codec parity.** The reference RRF / weighted fusion and
  the `f32-base64` vector codec now exist in **all four SDKs** — Python
  (`rcp.rrf_fuse` / `rcp.weighted_fuse` / `rcp.encode_vectors`), Node.js
  (`rrfFuse` / `weightedFuse` / `encodeVectors`), Rust (`rcp::fusion` /
  `rcp::vectors`), and C++ (`rcp::fusion` / `rcp::vectors`) — each unit-tested
  and byte-for-byte identical (same deterministic §16.3 tie-break, richest-body
  dedup, little-endian base64). The C++ live `Federation` now delegates to the
  standalone `rcp::fusion::rrf_fuse`, so one algorithm serves both paths.
- **Streaming (SSE) transport, end to end** (§9/§13). The Python `Server` gained
  `stream(method, generator)` and a `Progress` frame type: a handler `yield`s
  progress events and `return`s the final result, and `serve_http` now honours
  `Accept: text/event-stream` by emitting `notifications/progress` frames
  followed by one final response frame over a single connection. The same
  handler answers a plain unary POST with a single buffered response, exactly as
  §13 requires.
- **Runnable examples.** `examples/example_streaming.py` starts a real HTTP+SSE
  server and watches a 3-stage retrieve funnel fill live; `examples/example_federation.py`
  is a **one-command** demo that spawns two independent engine subprocesses,
  fans a query out to both, and fuses their rankings with `rcp.rrf_fuse` (origin
  tags + per-engine weights) — spec §16 made executable.

### Changed
- **The reference server is now a real hybrid pipeline.** `examples/example_server.py`
  was dense-only cosine; it now does dense + learned-sparse recall → RRF fusion
  → cross-encoder-style rerank → top-k, honouring the `candidateK ≥ topN ≥ k`
  funnel, returning per-stage `scores`, `usage` telemetry, and real citations,
  supporting `mode: dense|sparse|hybrid`, and **certifying at L2**. It is now a
  readable, dependency-free template for a production RAG engine.
- Conformance suite expanded to 26 checks (adds funnel-violation, hybrid-mode,
  and rerank-ordering checks); green and honest against the Python **and** C++
  reference servers (both certify L2).

## [1.0 · ed.2] — 2026 (hardening revision)

Interop-correctness and robustness pass. Additive on the wire: every new field
and error code is capability- or opt-in-gated, and no existing shape changed.

### Fixed (correctness — all four SDKs)
- **Notifications were being answered.** A JSON-RPC frame with no `id` now
  produces no reply in any SDK — previously every server emitted a spurious
  `id: null` response, desynchronising any pipelining client (§4.5).
- **A failed handshake unlocked the server.** `initialize` now marks the
  session initialized only on *successful* version negotiation; a rejected
  version (`-32002`) leaves gated methods returning `-32001` (§13).
- `notifications/cancel` carrying an `id` is now rejected as malformed rather
  than silently accepted.

### Added
- **Five error codes** (§12): `-32012` Unauthorized, `-32013` PayloadTooLarge,
  `-32014` Timeout, `-32015` NotFound, `-32016` Conflict — with normative
  retryability and overlap-disambiguation rules. Defined in all four SDKs.
- **Compact vector encoding** `f32-base64` (§7.3.1): negotiated, ~4× smaller
  than JSON numbers, always falling back to `json`. Reference codec in the
  Python SDK (`encode_vectors` / `decode_vectors`).
- **Lifecycle rules** (§13): per-request deadlines + `Timeout`, transport-death
  semantics, `index/add` idempotency (upsert-by-id), and a **capability-stability**
  guarantee (the set is fixed for a session).
- **Federation robustness** (§16.2): partial-failure semantics, hop-budget /
  path loop detection, and deadline propagation.
- **Authentication surface** (§15.6): where credentials live and how `-32012`
  must be used without leaking document existence.
- Shared param validation in every SDK: structurally invalid counts (`k <= 0`,
  non-integer) and funnel-invariant violations are `-32602`.

### Changed
- **The JSON Schema is now a real validator.** It previously defined 45 unused
  `$defs` with no root schema — it validated nothing. It now validates any
  message or batch, binds each method to its params/result, constrains error
  codes and request ids, and enforces the Content `data`-XOR-`uri` rule. Added
  the previously-missing `GraphResult`.
- Conformance harness expanded from 10 to 23 checks, including notification
  silence, failed-handshake lockout, and the retrieve funnel/ordering/id
  invariants. Verified green against the Python **and** C++ reference servers.

## [1.0 · ed.] — 2026 (editorial revision)

Clarifications and one notification rename. **No changes to any
request/response shape.**

### Changed
- Normative timestamp/date encoding, score-scale & comparability rules, and
  `trust.score ∈ [0,1]` (§4.6).
- `filter` field-type × operator value-typing table and empty-combinator
  handling (§8).
- Client `capabilities` semantics and tightened version-negotiation wording
  (§7.1).
- `embed` accepts Content blocks via `inputs`, with `texts` retained as a
  legacy synonym (§7.3).
- Explicit `strict` default and the `candidateK ≥ rerank.topN ≥ k` funnel
  invariant (§7.7).
- `progressToken` typing/uniqueness (§9) and JSON-RPC batch edge cases (§11).
- **Renamed** the log notification method `log` → `notifications/log`; the
  `notifications/*` namespace is now reserved (§4.5, §17.1). The `log`
  *capability* key is unchanged.

## [1.0] — 2026 (initial stable release)

The first stable release of the Retrieval Context Protocol.

### Added
- Core methods: `initialize`, `info`, `embed`, `embed/sparse`, `embed/multi`,
  `rerank`, `retrieve`, `query/transform`, `graph`, `index/add`, `index/delete`,
  `catalog/list`, `shutdown`, `notifications/cancel`, `ping`.
- Capability negotiation; stdio + HTTP(+SSE) transports.
- A Content/modality model for multimodal & visual-document retrieval.
- Metadata filtering, streaming/progress, `notifications/log` observability,
  pagination, batching.
- Structured errors with retryability; determinism (`seed` / `indexVersion`).
- A full threat model and federation (registry + RRF/weighted fusion).
- Native **C++**, **Python**, **Node.js**, and **Rust** SDKs.

[1.0 · ed.]: https://github.com/1ay1/rcp/blob/main/spec/rcp-1.0.md#appendix-e--change-log
[1.0]: https://github.com/1ay1/rcp/releases/tag/v1.0
