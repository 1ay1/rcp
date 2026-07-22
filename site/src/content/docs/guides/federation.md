---
title: Federation & Selector
description: Target one backend or fuse many — both layer on the core retrieve handshake with zero new server obligations.
sidebar:
  order: 2
---

Both routing models layer on the core `retrieve` + capability handshake — **zero
new server obligations** — and both can be driven from a single `rcp.json`
registry.

| Model | Use it when | API |
|-------|-------------|-----|
| **Selector** | you have several backends and want to pick **one** — by id, by required capability, or by priority with liveness fallback (lazy connect) | `rcp.Selector.load("rcp.json").select_primary()` |
| **Federation** | you want to query **all** reachable engines concurrently and fuse the ranked lists with Reciprocal Rank Fusion (origin-tagged hits) | `Federation::from_registry_file("rcp.json")` |

## The registry — `rcp.json`

```json
{
  "engines": [
    { "id": "docs", "transport": "stdio", "command": ["python3", "docs_server.py"], "priority": 10 },
    { "id": "web",  "transport": "http",  "url": "http://127.0.0.1:8000/rcp", "weight": 1.5 }
  ]
}
```

## Reciprocal Rank Fusion

Federation queries every reachable engine concurrently and fuses the ranked
lists with **Reciprocal Rank Fusion** (RRF, Cormack et al. 2009, `k = 60`).
Each hit is origin-tagged so the client knows which engine produced it, and
weighted fusion lets you bias trusted engines. The exact tie-break and dedup
rules are normative in the [specification](/reference/spec/#16-federation--querying-every-reachable-engine).

## Liveness

Selector connects lazily and, on priority selection, falls back to the next
live engine if the primary fails its connect + handshake. This makes a
multi-backend deployment resilient without the client writing any failover glue.
