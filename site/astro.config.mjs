// @ts-check
import { defineConfig } from "astro/config";
import starlight from "@astrojs/starlight";
import { rehypeBaseLinks } from "./plugins/rehype-base-links.mjs";
import { rehypeTableScroll } from "./plugins/rehype-table-scroll.mjs";

// RCP documentation site — Astro + Starlight.
// This single file drives theme, nav, SEO, and social — the RCP analogue
// of ACP's Mintlify docs.json. Deployed to GitHub Pages at /rcp.
export default defineConfig({
  site: "https://1ay1.github.io",
  base: "/rcp",
  trailingSlash: "ignore",
  markdown: {
    rehypePlugins: [rehypeBaseLinks, rehypeTableScroll],
  },
  integrations: [
    starlight({
      title: "RCP",
      tagline: "The Retrieval Context Protocol",
      description:
        "RCP is an open, versioned JSON-RPC 2.0 protocol that standardises how clients obtain retrieval context from any RAG engine — any language, any vendor. The retrieval companion to MCP and ACP.",
      logo: {
        light: "./src/assets/logo-light.svg",
        dark: "./src/assets/logo-dark.svg",
        replacesTitle: false,
      },
      favicon: "/favicon.svg",
      customCss: ["./src/styles/theme.css"],
      social: [
        {
          icon: "github",
          label: "GitHub",
          href: "https://github.com/1ay1/rcp",
        },
      ],
      editLink: {
        baseUrl: "https://github.com/1ay1/rcp/edit/main/site/",
      },
      lastUpdated: true,
      head: [
        {
          tag: "meta",
          attrs: { property: "og:image", content: "https://1ay1.github.io/rcp/og.png" },
        },
        {
          tag: "meta",
          attrs: { name: "twitter:card", content: "summary_large_image" },
        },
        {
          tag: "meta",
          attrs: { name: "twitter:image", content: "https://1ay1.github.io/rcp/og.png" },
        },
      ],
      sidebar: [
        {
          label: "Start Here",
          items: [
            { label: "Introduction", slug: "start/introduction" },
            { label: "Architecture", slug: "start/architecture" },
            { label: "Quickstart", slug: "start/quickstart" },
            { label: "The method set", slug: "start/methods" },
          ],
        },
        {
          label: "Guides",
          items: [
            { label: "SDKs", slug: "guides/sdks" },
            { label: "Federation & Selector", slug: "guides/federation" },
            { label: "Errors & retryability", slug: "guides/errors" },
            { label: "Conformance", slug: "guides/conformance" },
          ],
        },
        {
          label: "Reference",
          items: [
            { label: "Specification — RCP/1", slug: "reference/spec", badge: { text: "Stable", variant: "success" } },
            {
              label: "JSON Schema",
              link: "/schema/rcp-1.0.json",
              attrs: { target: "_blank" },
            },
          ],
        },
        {
          label: "Ecosystem",
          items: [
            { label: "The agent stack", slug: "ecosystem/stack" },
            { label: "Relationship to MCP & ACP", slug: "ecosystem/mcp-acp" },
          ],
        },
      ],
    }),
  ],
});
