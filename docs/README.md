# RCP documentation site

The [Retrieval Context Protocol](https://github.com/1ay1/rcp) documentation,
built with [Mintlify](https://mintlify.com). Every protocol concept has its own
readable page; the normative source of truth remains
[`spec/rcp-1.0.md`](../spec/rcp-1.0.md) and
[`schema/rcp-1.0.json`](../schema/rcp-1.0.json) at the repo root.

## Local preview

Mintlify's CLI needs a Node LTS release (≤ 24 at time of writing):

```sh
npm i -g mint
cd docs
mint dev            # http://localhost:3000
```

## Validate (no CLI required)

These checks run in CI and need only Node — no Mintlify install:

```sh
cd docs
node scripts/check-nav.mjs      # every docs.json page resolves to a file
node scripts/check-links.mjs    # every internal link resolves
node scripts/check-mdx.mjs      # balanced components, no stray < / { in prose
node scripts/sync-schema.mjs    # refresh docs/schema from the canonical schema
```

## Structure

- `docs.json` — Mintlify config: theme, colors, and the tab → group → page navigation.
- `get-started/`, `concepts/`, `methods/`, `features/`, `operating/` — the Documentation tab.
- `sdks/` — the SDKs tab (C++, Python, Node.js, Rust).
- `reference/`, `ecosystem/` — the Reference tab.
- `schema/rcp-1.0.json` — the JSON Schema, served at `/schema/rcp-1.0.json` (synced from the repo root).
- `logo/`, `favicon.svg`, `images/` — brand assets.

## Deployment

Hosting is via the **Mintlify GitHub App**: connect the `1ay1/rcp` repo in the
[Mintlify dashboard](https://dashboard.mintlify.com) and point it at the `docs/`
directory. Every push to `main` then deploys automatically — no build workflow in
this repo. The `.github/workflows/docs.yml` workflow only validates the docs on
pull requests.
