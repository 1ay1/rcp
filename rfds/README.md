# RFDs — Requests for Discussion

RCP evolves through **RFDs**. An RFD is a short design document that proposes a
change to the protocol, schema, or capability lattice and records the discussion
around it. This mirrors how mature protocols (and Oxide's RFD process) keep a
durable, reviewable trail of *why* the wire looks the way it does.

## When you need one

| Change | RFD required? |
|--------|---------------|
| Typo / wording / doc clarification | No — just open a PR |
| SDK bug fix (no wire change) | No |
| New method, capability, error code, or field | **Yes** |
| Any change to an existing wire/schema shape | **Yes** (and it must stay additive) |

## Process

1. Copy [`0000-template.md`](0000-template.md) to `rfds/NNNN-short-title.md`
   (pick the next free number).
2. Open a PR with the RFD in `Draft`. Discussion happens on the PR.
3. When consensus forms, a maintainer marks it `Accepted` and it lands in a
   numbered protocol version. See
   [Governance](https://rcp-6d6ef6d5.mintlify.site/community/governance).

The golden rule: **the wire never breaks under existing peers.** Every accepted
change is additive and discovered through capability negotiation.
