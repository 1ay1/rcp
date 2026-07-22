---
title: Conformance
description: The three named conformance levels — L0 Base, L1 Retrieval, L2 SOTA — and the transport-agnostic test suite.
sidebar:
  order: 4
---

RCP defines three named conformance levels. A server **MUST** state its level in
`info`/`initialize` via `_meta.conformance` (`"L0"`, `"L1"`, or `"L2"`); clients
use it as a coarse expectation, and capabilities as the precise contract.

## Levels

### L0 — Base *(REQUIRED of every server)*

Answer `initialize`, `info`, and `ping` (the latter two at any time, echoing a
`ping` nonce); advertise at least one retrieval capability; reject
pre-`initialize` requests with `-32001`; reject unadvertised capability methods
(`-32003`), undefined methods (`-32004`), and unsupported options (`-32005`);
return well-formed JSON-RPC 2.0 echoing the request `id`; validate params
(`-32602`) instead of crashing; ignore unknown notifications; tolerate unknown
fields, capability keys, and `_meta`.

### L1 — Retrieval *(RECOMMENDED)*

L0 **plus** `retrieve` with `modes:["dense","sparse","hybrid"]`, `filter` and
`citations` support, consistent `Hit.score` (higher = more relevant), and
`k`/`minScore` honoured. The target for a general-purpose RAG backend.

### L2 — SOTA *(OPTIONAL)*

L1 **plus** a selection of `rerank` (cross-encoder and/or ColBERT),
`multiVector` / late interaction (incl. visual-document `modalities:["image"]`),
`sparseEmbed`, `query/transform`, `graph`, `streaming`, `pagination`, `log`, and
reproducibility (`deterministic`/`snapshots`). L2 lets a client drive a full
hybrid → fuse → rerank → MMR pipeline through one connection.

## The test suite

The JSON Schema in [`/schema`](/schema/rcp-1.0.json) is normative for message
shapes; the transport-agnostic conformance suite validates **any** server, in
**any** language, over stdio or HTTP:

```sh
# Validate a Python engine
python3 conformance/check.py -- python3 examples/example_server.py

# Validate a C++ engine
python3 conformance/check.py -- ./sdk/cpp/example_server
```

Both reference example servers pass **10/10**. A conforming *client* must send
`initialize` first, honour the negotiated version, never call an unadvertised
method, correlate responses by `id`, and function correctly even if it drops
every `notifications/progress` and `notifications/log` notification.
