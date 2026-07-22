# RCP documentation site

The [rcp](https://1ay1.github.io/rcp/) documentation site — built with
[Astro](https://astro.build) + [Starlight](https://starlight.astro.build).

The **specification** (`spec/rcp-1.0.md`) and **JSON Schema**
(`schema/rcp-1.0.json`) at the repo root remain the single source of truth.
On every build, `scripts/sync-spec.mjs` renders the spec into
`src/content/docs/reference/spec.md`, `scripts/sync-schema.mjs` copies the schema
into `public/schema/`, and `scripts/gen-og.mjs` regenerates the social card —
so the site can never drift from the normative documents.

## Local development

```sh
cd site
npm install
npm run dev      # http://localhost:4321/rcp
```

## Build

```sh
npm run build    # syncs spec + schema + OG, then astro build -> dist/
npm run preview  # serve the built site locally
```

## Deployment

Fully automated. A push to `main` that touches `site/`, `spec/rcp-1.0.md`, or
`schema/rcp-1.0.json` triggers `.github/workflows/deploy-site.yml`, which builds
the site and publishes `dist/` to GitHub Pages. No manual steps.

> One-time setup: in the repo's **Settings → Pages**, set **Source** to
> **GitHub Actions**.

## Structure

- `astro.config.mjs` — the single config that drives theme, nav, SEO, social
  (the RCP analogue of ACP's Mintlify `docs.json`).
- `src/content/docs/` — the content tree (MDX/Markdown).
- `src/styles/theme.css` — RCP brand theming (indigo/violet).
- `plugins/rehype-base-links.mjs` — base-prefixes root-absolute internal links.
- `scripts/` — spec/schema/OG sync run before each build.
