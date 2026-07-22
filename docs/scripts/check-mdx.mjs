// check-mdx.mjs — lightweight MDX sanity checks (we can't run `mint` on Node 26):
//   1. balanced block components (<Tag>…</Tag>)
//   2. stray `<` or `{` in prose (outside code) that MDX would try to parse as JSX
// Run: node scripts/check-mdx.mjs
import { readFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { listFiles } from "./lib.mjs";

const root = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const files = listFiles(root, ".mdx");

const BLOCK = [
  "CardGroup", "Card", "Steps", "Step", "Tabs", "Tab", "AccordionGroup",
  "Accordion", "CodeGroup", "ParamField", "ResponseField", "Note", "Warning",
  "Tip", "Info", "Update", "Expandable", "Frame",
];

function stripCode(s) {
  return s
    .replace(/^---[\s\S]*?\n---/, "") // frontmatter
    .replace(/```[\s\S]*?```/g, "")   // fenced blocks
    .replace(/`[^`]*`/g, "");          // inline code (may wrap one soft break)
}

let problems = 0;
for (const f of files) {
  const raw = readFileSync(path.join(root, f), "utf8");
  const prose = stripCode(raw);
  const proseNoTags = prose.replace(/<[^>]*>/g, ""); // also drop JSX/HTML tags

  for (const tag of BLOCK) {
    const opens = (prose.match(new RegExp(`<${tag}\\b(?:[^>]*?)>`, "g")) || [])
      .filter((t) => !t.endsWith("/>")).length;
    const closes = (prose.match(new RegExp(`</${tag}>`, "g")) || []).length;
    if (opens !== closes) {
      console.error(`  UNBALANCED <${tag}> in ${f}: ${opens} open, ${closes} close`);
      problems++;
    }
  }

  proseNoTags.split("\n").forEach((line, i) => {
    // a `<` not followed by a letter or `/` is likely a literal in prose
    const badLt = /<(?![A-Za-z/!])/.test(line.replace(/&lt;|&gt;/g, ""));
    if (badLt) { console.error(`  STRAY '<' ${f}:${i + 1}: ${line.trim().slice(0, 70)}`); problems++; }
    if (line.includes("{")) { console.error(`  STRAY '{' ${f}:${i + 1}: ${line.trim().slice(0, 70)}`); problems++; }
  });
}

console.log(`mdx files: ${files.length}   problems: ${problems}`);
process.exit(problems ? 1 : 0);
