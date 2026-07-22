---
title: Architecture
description: Roles, the retrieval pipeline model, and how RCP sits beside MCP and ACP.
sidebar:
  order: 2
---

An agent runtime speaks **ACP** to its client, calls tools over **MCP**, and
fetches grounding context over **RCP**. All three share JSON-RPC framing, an
`initialize`/capabilities handshake, `_meta` extensibility, and cursor
pagination — learn one, you know the shape of all three.

```
  client  ⇄  ACP  ⇄  agent  ⇄  MCP  ⇄  tools
                       │
                       └──────  RCP  ⇄  retrieval engine(s)
```

## Roles

- **Client** — an application, agent runtime, or IDE that needs retrieval
  context. Initiates the connection and drives the handshake.
- **Server (engine)** — a RAG engine, vector database, or aggregator that
  advertises capabilities and answers method calls.

The client always initiates. A server MUST NOT assume any capability was
negotiated until `initialize` completes.

## The retrieval pipeline model

RCP models retrieval as a composable pipeline. A single `retrieve` call can
transparently drive as many stages as the engine supports:

1. **Query transform** — expansion, HyDE, decomposition (`transform`).
2. **Candidate generation** — dense, sparse, or hybrid first-stage recall.
3. **Rerank** — cross-encoder or late-interaction (MaxSim) scoring.
4. **Diversify** — MMR to reduce redundancy.
5. **Assemble** — apply metadata filters, recency, and attach citations.

A thin client just calls `retrieve` and reads hits; a sophisticated client can
also call the individual stages (`embed`, `rerank`, `query/transform`, `graph`)
directly.

## Message shape

Every message is JSON-RPC 2.0. Requests carry an `id`; notifications do not.
Extensibility rides in `_meta`. See the
[specification](/reference/spec/#4-message-format) for the normative details.
