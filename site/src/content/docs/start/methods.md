---
title: The method set
description: Every RCP method, its gating capability, and its purpose.
sidebar:
  order: 4
---

Every RCP method is gated by a capability the server advertises during
`initialize`. Clients check `supports(...)` before calling — failures surface
*client-side* as `CapabilityMissing` before any I/O.

| Method | Capability | Purpose |
|--------|-----------|---------|
| `initialize` / `info` | always | version + capability handshake / stateless identity |
| `embed` | `embed` | dense embeddings |
| `embed/sparse` | `sparseEmbed` | learned-sparse terms (SPLADE) |
| `embed/multi` | `multiVector` | per-token vectors (ColBERT) |
| `rerank` | `rerank` | cross-encoder / late-interaction scoring |
| `retrieve` | `retrieve` | the workhorse — mode, filter, rerank, MMR, recency, citations |
| `query/transform` | `transform` | expansion / HyDE / decomposition |
| `graph` | `graph` | GraphRAG local / global / drift |
| `index/add`, `index/delete` | `index` | mutate the corpus |
| `catalog/list` | `catalog` | enumerate federated engines |
| `notifications/cancel` | always | abandon an in-flight request |
| `ping` | always | liveness / round-trip; echoes a nonce |
| `shutdown` | always | graceful close |

`retrieve` is the workhorse: a single call can drive query transform, hybrid
recall, rerank, MMR diversification, recency weighting, metadata filtering, and
citation assembly — whatever the engine advertises.

Full request/response shapes, error codes, streaming, and pagination rules are
in the [specification](/reference/spec/#7-methods).
