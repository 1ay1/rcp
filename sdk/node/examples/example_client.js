#!/usr/bin/env node
// example_client.js — connect to the example server and use every capability.
import path from "node:path";
import { fileURLToPath } from "node:url";
import * as rcp from "../src/index.js";

const here = path.dirname(fileURLToPath(import.meta.url));

async function main() {
  const server = path.join(here, "example_server.js");
  const c = await rcp.connectStdio(["node", server]);

  console.log(`connected to ${c.server.name} v${c.server.version} (RCP/${c.protocolVersion})`);
  console.log("capabilities:", Object.keys(c.capabilities));

  if (c.supports(rcp.Capability.Embed)) {
    const vecs = await c.embed(["hello world"]);
    console.log(`embed      -> ${vecs.length} vector(s), dim=${vecs[0].length}`);
  }

  if (c.supports(rcp.Capability.Retrieve)) {
    console.log("retrieve   ->");
    for (const h of await c.retrieve("landmark in the French capital", 2)) {
      console.log(`   ${h.id}  score=${h.score.toFixed(3)}  ${h.text.slice(0, 48)}`);
    }
  }

  if (c.supports(rcp.Capability.Graph)) {
    const g = await c.graph("global", { query: "anything" });
    console.log("graph      ->", g);
  }

  await c.shutdown();
}

main();
