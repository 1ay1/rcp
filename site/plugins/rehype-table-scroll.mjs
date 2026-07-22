// Rehype plugin: wrap every <table> in <div class="rcp-table-scroll"> so wide
// spec tables scroll horizontally instead of overflowing the content column.
import { visit } from "unist-util-visit";

export function rehypeTableScroll() {
  return (tree) => {
    visit(tree, "element", (node, index, parent) => {
      if (node.tagName !== "table" || !parent || index == null) return;
      // avoid double-wrapping
      if (parent.type === "element" && parent.properties?.className?.includes?.("rcp-table-scroll")) return;
      const wrapper = {
        type: "element",
        tagName: "div",
        properties: { className: ["rcp-table-scroll"] },
        children: [node],
      };
      parent.children[index] = wrapper;
    });
  };
}
