// Rehype plugin: prefix Astro's `base` onto root-absolute internal links.
// Lets content authors write [x](/start/foo/) and have it resolve to /rcp/... .
// Skips external links, anchors, asset paths, and links already under base.
import { visit } from "unist-util-visit";

const BASE = "/rcp";
// Internal top-level route segments that should be base-prefixed.
const INTERNAL = /^\/(start|guides|reference|ecosystem|schema)(\/|#|$)/;

export function rehypeBaseLinks() {
  return (tree) => {
    visit(tree, "element", (node) => {
      if (node.tagName !== "a") return;
      const href = node.properties?.href;
      if (typeof href !== "string") return;
      if (href.startsWith(BASE + "/") || href === BASE) return;
      if (INTERNAL.test(href)) {
        node.properties.href = BASE + href;
      }
    });
  };
}
