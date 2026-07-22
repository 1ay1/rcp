// check-links.mjs — verify internal doc links resolve to real pages/assets.
// Run: node scripts/check-links.mjs
import { readFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { listFiles } from "./lib.mjs";

const root = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const files = listFiles(root, ".mdx");
const pageSet = new Set(files.map((f) => "/" + f.replace(/\.mdx$/, "")));
pageSet.add("/index"); // "/" resolves to index

// Static asset prefixes that are served as-is.
const assetPrefix = ["/images/", "/schema/", "/logo/", "/favicon"];

const linkRe = /\]\((\/[^)]*)\)|href="(\/[^"]*)"/g;
let broken = 0;
let checked = 0;

for (const f of files) {
  const text = readFileSync(path.join(root, f), "utf8");
  let m;
  while ((m = linkRe.exec(text))) {
    let target = (m[1] || m[2]).split("#")[0];
    if (!target || target === "/") continue;
    checked++;
    if (assetPrefix.some((p) => target.startsWith(p))) continue;
    const norm = target.replace(/\/$/, "");
    if (!pageSet.has(norm)) {
      console.error(`  BROKEN: ${target}   (in ${f})`);
      broken++;
    }
  }
}

console.log(`internal links checked: ${checked}   broken: ${broken}`);
process.exit(broken ? 1 : 0);
