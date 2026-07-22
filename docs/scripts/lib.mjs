// lib.mjs — tiny shared helpers for the docs validation scripts.
//
// A dependency-free recursive file lister. We deliberately avoid `fs.globSync`
// (only stable since Node 22) so the checks run on the Node 20 LTS used in CI
// and on any contributor's machine.
import { readdirSync } from "node:fs";
import path from "node:path";

/**
 * Recursively list files under `root` whose name ends with `ext` (e.g. ".mdx").
 * Returns forward-slash paths relative to `root`, sorted. Skips dotfiles and
 * node_modules so output matches the old `globSync("**\/*.mdx")` behaviour.
 */
export function listFiles(root, ext) {
  const out = [];
  (function walk(dir, rel) {
    for (const entry of readdirSync(dir, { withFileTypes: true })) {
      if (entry.name.startsWith(".") || entry.name === "node_modules") continue;
      const abs = path.join(dir, entry.name);
      const relPath = rel ? `${rel}/${entry.name}` : entry.name;
      if (entry.isDirectory()) walk(abs, relPath);
      else if (entry.name.endsWith(ext)) out.push(relPath);
    }
  })(root, "");
  return out.sort();
}
