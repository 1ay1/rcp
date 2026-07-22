---
title: Relationship to MCP & ACP
description: How RCP reuses the JSON-RPC conventions of MCP and ACP, and why retrieval deserves its own protocol.
sidebar:
  order: 2
---

RCP is a deliberate sibling of MCP and ACP, not a competitor. It reuses their
proven JSON-RPC conventions so an implementer of one is instantly productive in
another.

## What RCP shares

- **JSON-RPC 2.0 framing** — requests with `id`, notifications without.
- **`initialize` + capability negotiation** — a server advertises exactly what
  it supports; clients gate calls on capabilities.
- **`_meta` extensibility** — additive fields ride in `_meta` without breaking
  the wire.
- **Cursor-based pagination** — the same opaque-cursor model.
- **Markdown-first human text** and reused MCP JSON representations where
  sensible.

## Why retrieval needs its own protocol

Retrieval is not just "another tool call." It has a rich, well-studied shape that
a generic tool interface cannot express faithfully:

- **A pipeline, not a function** — transform → recall → rerank → diversify →
  cite, with each stage independently negotiable.
- **Ranking semantics** — scores, ranks, and fusion (RRF) are first-class,
  because federating heterogeneous engines is the norm.
- **A distinct threat model** — retrieved content is *untrusted data moving
  toward an LLM's context*, dominated by indirect prompt injection and
  provenance concerns that ordinary RPC never faces.

Modelling retrieval as a plain MCP tool would erase all of this. RCP gives the
retrieval layer the same first-class, vendor-neutral contract that MCP gave tools
and ACP gave agents.

See [Appendix C](/reference/spec/#appendix-c--relationship-to-mcp-and-acp) of the
specification for the normative summary.
