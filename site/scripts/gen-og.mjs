#!/usr/bin/env node
/* Generate the 1200x630 social/OG card into public/og.png using sharp. */
import { mkdirSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import sharp from "sharp";

const here = dirname(fileURLToPath(import.meta.url));
const OUT = resolve(here, "../public/og.png");

const svg = `<svg width="1200" height="630" viewBox="0 0 1200 630" xmlns="http://www.w3.org/2000/svg">
  <defs>
    <linearGradient id="bg" x1="0" y1="0" x2="1200" y2="630" gradientUnits="userSpaceOnUse">
      <stop stop-color="#0b0f0e"/>
      <stop offset="1" stop-color="#0a1614"/>
    </linearGradient>
    <linearGradient id="g" x1="0" y1="0" x2="1" y2="1">
      <stop stop-color="#17e6c3"/>
      <stop offset="0.55" stop-color="#34d399"/>
      <stop offset="1" stop-color="#ffb454"/>
    </linearGradient>
    <radialGradient id="aura" cx="0.28" cy="0.3" r="0.7">
      <stop stop-color="#17e6c3" stop-opacity="0.22"/>
      <stop offset="1" stop-color="#17e6c3" stop-opacity="0"/>
    </radialGradient>
  </defs>
  <rect width="1200" height="630" fill="url(#bg)"/>
  <rect width="1200" height="630" fill="url(#aura)"/>
  <!-- retrieval mark -->
  <g transform="translate(96,232) scale(5.2)">
    <circle cx="6" cy="8" r="3" fill="url(#g)"/>
    <circle cx="6" cy="16" r="3" fill="url(#g)"/>
    <circle cx="6" cy="24" r="3" fill="url(#g)"/>
    <path d="M9 8 Q20 8 24 16 Q20 24 9 24 M9 16 H21" stroke="url(#g)" stroke-width="2" fill="none" stroke-linecap="round"/>
    <circle cx="26" cy="16" r="4.5" fill="url(#g)"/>
  </g>
  <text x="96" y="330" font-family="Space Grotesk, Inter, Helvetica, Arial, sans-serif" font-size="112" font-weight="700" fill="#ffffff" letter-spacing="-3">RCP</text>
  <text x="100" y="400" font-family="Space Grotesk, Inter, Helvetica, Arial, sans-serif" font-size="40" font-weight="600" fill="#a7ffee">The Retrieval Context Protocol</text>
  <text x="100" y="452" font-family="Inter, Helvetica, Arial, sans-serif" font-size="26" fill="#b6c0c0">One open JSON-RPC contract for any RAG engine.</text>
  <text x="100" y="490" font-family="Inter, Helvetica, Arial, sans-serif" font-size="26" fill="#b6c0c0">The retrieval companion to MCP and ACP.</text>
  <text x="96" y="580" font-family="JetBrains Mono, monospace" font-size="24" fill="#17e6c3">RCP/1 · Stable · JSON-RPC 2.0</text>
</svg>`;

mkdirSync(dirname(OUT), { recursive: true });
await sharp(Buffer.from(svg)).png().toFile(OUT);
console.log(`[og] wrote ${OUT}`);
