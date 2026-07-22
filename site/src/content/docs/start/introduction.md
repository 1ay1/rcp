---
title: Introduction
description: What RCP is, who it's for, and the problem it solves.
sidebar:
  order: 1
---

Every RAG stack reinvents the same wire: **embed, rerank, retrieve, filter, cite.**
Swapping a vector database, adding a reranker, or wiring in a second knowledge
base means rewriting glue code. MCP standardised *tools*; ACP standardised
*agents*; **nothing standardised the retrieval layer that grounds them.**

**RCP is that layer** — a small, versioned, JSON-RPC 2.0 contract that a
*powerful* engine can fully express and a *thin* client can fully consume.

## The four pillars

- **Capability-negotiated.** A server advertises exactly what it can do
  (`embed`, `sparseEmbed`, `multiVector`, `rerank`, `retrieve`, `transform`,
  `graph`, `index`, `catalog`). Clients gate calls on capabilities — no probing,
  no surprises.
- **SOTA-complete.** Hybrid (dense + learned-sparse) search, ColBERT/ColPali
  late-interaction, SPLADE, multi-stage rerank cascades, MMR diversification,
  contextual retrieval, GraphRAG (local/global/drift), agentic multi-hop,
  citations, streaming, and metadata filtering are all first-class.
- **Transport-free.** JSON-RPC 2.0 over newline-delimited stdio *or* HTTP. If it
  can read a line and parse JSON, it can speak RCP.
- **Composable.** A registry (`rcp.json`) or a `catalog`-capable aggregator lets
  a client target one backend (**Selector**) or fuse many (**Federation**).

## Who is this for?

- **Engine authors** — implement one server and reach every RCP client.
- **Agent & IDE builders** — pull grounding context from any engine without
  bespoke adapters.
- **Platform teams** — federate several knowledge bases behind one uniform wire.

## Next

- [Architecture](/start/architecture/) — roles and the pipeline model.
- [Quickstart](/start/quickstart/) — a working engine in minutes.
- [Specification](/reference/spec/) — the complete normative document.
