// check-config.mjs — lint docs.json for Mintlify build-breakers that neither
// JSON-Schema validation nor the other checks catch (they only surface at build
// time). Dependency-free so it runs on the CI's Node 20. Run: node scripts/check-config.mjs
import { readFileSync, existsSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const cfgPath = path.join(root, "docs.json");

let cfg;
try {
  cfg = JSON.parse(readFileSync(cfgPath, "utf8"));
} catch (e) {
  console.error("  docs.json is not valid JSON:", e.message);
  process.exit(1);
}

const problems = [];

// --- Fonts -----------------------------------------------------------------
// `format` describes a SELF-HOSTED font and is only valid alongside `source`.
// Google Fonts (the common case) take `family` alone. A `format` without a
// `source` makes Mintlify try to load a self-hosted file that doesn't exist and
// the build fails — exactly the bug that froze this site. (See MEMORY.)
function checkFont(where, font) {
  if (!font || typeof font !== "object") return;
  if (font.format && !font.source) {
    problems.push(
      `fonts.${where}: has "format":"${font.format}" but no "source". ` +
        `"format" is only for self-hosted fonts; for a Google Font (e.g. Lora, Inter) ` +
        `give "family" only and drop "format".`
    );
  }
  if (font.source && !font.format) {
    problems.push(
      `fonts.${where}: has a "source" but no "format" (woff|woff2) — Mintlify requires "format" when "source" is set.`
    );
  }
}
if (cfg.fonts) {
  if (cfg.fonts.family || cfg.fonts.source || cfg.fonts.format) checkFont("(root)", cfg.fonts);
  checkFont("heading", cfg.fonts.heading);
  checkFont("body", cfg.fonts.body);
}

// --- Referenced local assets exist -----------------------------------------
// A logo/favicon/og:image pointing at a missing file is a build-time failure.
const assetRefs = [];
const pushAsset = (val, label) => {
  if (typeof val === "string" && val.startsWith("/") && /\.(svg|png|jpe?g|ico|webp|gif|woff2?)$/i.test(val)) {
    assetRefs.push({ val, label });
  }
};
if (typeof cfg.logo === "string") pushAsset(cfg.logo, "logo");
if (cfg.logo && typeof cfg.logo === "object") {
  pushAsset(cfg.logo.light, "logo.light");
  pushAsset(cfg.logo.dark, "logo.dark");
}
if (cfg.favicon && typeof cfg.favicon === "object") {
  pushAsset(cfg.favicon.light, "favicon.light");
  pushAsset(cfg.favicon.dark, "favicon.dark");
} else pushAsset(cfg.favicon, "favicon");
pushAsset(cfg.seo?.metatags?.["og:image"], "seo.metatags.og:image");
// self-hosted font sources, if any
for (const w of ["heading", "body"]) pushAsset(cfg.fonts?.[w]?.source, `fonts.${w}.source`);

for (const { val, label } of assetRefs) {
  if (!existsSync(path.join(root, val.replace(/^\//, "")))) {
    problems.push(`${label}: references "${val}" but that file does not exist under docs/.`);
  }
}

// --- Redirects are well-formed ---------------------------------------------
if (Array.isArray(cfg.redirects)) {
  cfg.redirects.forEach((r, i) => {
    if (!r || typeof r !== "object" || !r.source || !r.destination) {
      problems.push(`redirects[${i}]: needs both "source" and "destination".`);
    } else if (!r.source.startsWith("/") || !r.destination.startsWith("/")) {
      problems.push(`redirects[${i}]: source and destination must be root-relative paths (start with "/").`);
    }
  });
}

// --- Report ----------------------------------------------------------------
if (problems.length) {
  console.error(`docs.json config: ${problems.length} problem(s):`);
  for (const p of problems) console.error("  -", p);
  process.exit(1);
}
console.log("docs.json config: OK (fonts, assets, redirects)");
