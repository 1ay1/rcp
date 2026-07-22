// check-nav.mjs — verify every page in docs.json resolves to a file, and flag
// any orphan .mdx pages not referenced in the navigation. Run: node scripts/check-nav.mjs
import { readFileSync, existsSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { listFiles } from "./lib.mjs";

const root = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const cfg = JSON.parse(readFileSync(path.join(root, "docs.json"), "utf8"));

const pages = [];
function walk(node) {
  if (Array.isArray(node)) { node.forEach(walk); return; }
  if (node && typeof node === "object") {
    if (Array.isArray(node.tabs)) node.tabs.forEach(walk);
    if (Array.isArray(node.groups)) node.groups.forEach(walk);
    if (Array.isArray(node.pages)) node.pages.forEach(walk);
    return;
  }
  if (typeof node === "string") pages.push(node);
}
walk(cfg.navigation);

let missing = 0;
for (const p of pages) {
  const mdx = path.join(root, p + ".mdx");
  const md = path.join(root, p + ".md");
  if (!existsSync(mdx) && !existsSync(md)) {
    console.error("  MISSING:", p);
    missing++;
  }
}

const onDisk = listFiles(root, ".mdx").map((f) => f.replace(/\.mdx$/, ""));
const referenced = new Set(pages);
const orphans = onDisk.filter((f) => !referenced.has(f));

console.log(`nav pages: ${pages.length}   files: ${onDisk.length}   missing: ${missing}   orphans: ${orphans.length}`);
if (orphans.length) console.log("  orphans (not in nav):", orphans.join(", "));
process.exit(missing ? 1 : 0);
