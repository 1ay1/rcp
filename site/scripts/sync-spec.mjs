#!/usr/bin/env node
/* Sync the canonical spec markdown into the Starlight docs tree.
 *
 * Source of truth: ../../spec/rcp-1.0.md  (repo root spec/)
 * Output:          ../src/content/docs/reference/spec.md
 *
 * Transforms:
 *  - Drop the top H1 + metadata block + manual Table of Contents
 *    (Starlight renders the title from frontmatter and auto-builds a TOC).
 *  - Prepend Starlight frontmatter.
 *  - Rewrite the relative schema link (../schema/…) to the served path.
 *
 * Run automatically before every build (see package.json / CI). Also run
 * manually with `npm run sync`.
 */
import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const SRC = resolve(here, "../../spec/rcp-1.0.md");
const OUT = resolve(here, "../src/content/docs/reference/spec.md");

let md = readFileSync(SRC, "utf8");

// Split off everything up to and including the "## Table of Contents" block.
// The spec structure is:  # title / metadata / blockquote / --- / ## Table of Contents / ...items... / --- / ## Abstract ...
// We keep from the first "## Abstract" onward, and re-add the RFC-2119 note + metadata as intro.
const rfc2119Match = md.match(/> The key words \*\*MUST\*\*[\s\S]*?appear in all capitals, as shown here\./);
const rfc2119 = rfc2119Match ? rfc2119Match[0] : "";

const abstractIdx = md.indexOf("## Abstract");
if (abstractIdx === -1) {
  throw new Error("Could not find '## Abstract' in spec — sync aborted.");
}
let body = md.slice(abstractIdx);

// Rewrite relative schema link to the served location.
body = body.replaceAll("](../schema/rcp-1.0.json)", "](/rcp/schema/rcp-1.0.json)");
body = body.replaceAll("(spec/rcp-1.0.md#", "(#"); // internal cross-refs that pointed at the file

const frontmatter = `---
title: Specification — RCP/1
description: The complete normative specification for the Retrieval Context Protocol, version 1.0.
tableOfContents:
  minHeadingLevel: 2
  maxHeadingLevel: 3
sidebar:
  order: 1
---

:::note[Normative document · RCP/1 · Stable]
**Version:** 1.0 (\`RCP/1\`) · **Status:** Stable · **Normative schema:** [\`/schema/rcp-1.0.json\`](/rcp/schema/rcp-1.0.json)

${rfc2119.replace(/^> /gm, "")}
:::

`;

mkdirSync(dirname(OUT), { recursive: true });
writeFileSync(OUT, frontmatter + body, "utf8");

console.log(`[sync-spec] wrote ${OUT} (${body.length} bytes of spec body)`);
