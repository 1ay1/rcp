<!-- Thanks for contributing to RCP! -->

## What & why

<!-- What does this change, and why? Link the issue/RFD it addresses. -->

Closes #

## Type of change

- [ ] Spec / schema (normative) — **requires an RFD**
- [ ] SDK (C++ / Python / Node / Rust)
- [ ] Documentation site
- [ ] Conformance / tooling
- [ ] Chore / CI

## Checklist

- [ ] The four SDKs stay **wire-compatible** (any client ↔ any server).
- [ ] Spec, schema, SDKs, and docs are consistent with each other.
- [ ] If normative: the change is additive and capability-gated (the wire does not break).
- [ ] Docs validate: `cd docs && node scripts/sync-schema.mjs && node scripts/check-nav.mjs && node scripts/check-links.mjs && node scripts/check-mdx.mjs`.
- [ ] Relevant SDK tests / conformance pass.
- [ ] No new third-party dependencies added to the SDKs.
