// sync-schema.mjs — copy the canonical JSON Schema into the docs so it is served
// at /schema/rcp-1.0.json. The spec (spec/rcp-1.0.md) and schema
// (schema/rcp-1.0.json) at the repo root stay the single source of truth.
// Run from docs/: node scripts/sync-schema.mjs
import { copyFileSync, mkdirSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const docs = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const repo = path.dirname(docs);
const src = path.join(repo, "schema", "rcp-1.0.json");
const dstDir = path.join(docs, "schema");
mkdirSync(dstDir, { recursive: true });
copyFileSync(src, path.join(dstDir, "rcp-1.0.json"));
console.log("synced schema/rcp-1.0.json -> docs/schema/rcp-1.0.json");
