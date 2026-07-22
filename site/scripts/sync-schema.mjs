#!/usr/bin/env node
/* Copy the canonical JSON Schema into the site's public/ so it is served at
 * /rcp/schema/rcp-1.0.json — keeping repo-root schema/ as the source of truth.
 */
import { copyFileSync, mkdirSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const SRC = resolve(here, "../../schema/rcp-1.0.json");
const OUT = resolve(here, "../public/schema/rcp-1.0.json");

mkdirSync(dirname(OUT), { recursive: true });
copyFileSync(SRC, OUT);
console.log(`[sync-schema] copied schema -> ${OUT}`);
