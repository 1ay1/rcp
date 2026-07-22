# Changelog

All notable changes to the Retrieval Context Protocol are documented here.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

RCP/1 is **stable**. Changes within the major version are additive and
capability-discovered — the wire never breaks under you.

The authoritative change log is
[Appendix E of the specification](spec/rcp-1.0.md#appendix-e--change-log); the
rendered version lives at the
[Changelog page](https://rcp-6d6ef6d5.mintlify.site/reference/changelog).

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
