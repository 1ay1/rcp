// rcp — the Retrieval Context Protocol, a native Node.js SDK.
//
// Zero dependencies (Node standard library only). RCP is an open, versioned
// JSON-RPC protocol so any RAG engine — any language, any vendor — can expose
// embed / rerank / retrieve / graph / index / catalog, and any client can
// consume it uniformly. This SDK speaks the exact same wire format as the
// type-theoretic C++ SDK and the native Python SDK, so a Node client and a
// C++/Python server (or vice-versa) interoperate byte-for-byte.
//
// Client (connects to any RCP server, over a subprocess or HTTP):
//
//     import * as rcp from "rcp-protocol";
//     const c = await rcp.connectStdio(["node", "my_server.js"]);
//     console.log(c.server, c.capabilities);
//     if (c.supports(rcp.Capability.Retrieve))
//       for (const hit of await c.retrieve("eiffel tower", 3))
//         console.log(hit.id, hit.score);
//
// Server (expose a Node RAG engine as an RCP server):
//
//     import * as rcp from "rcp-protocol";
//     const s = new rcp.Server();
//     s.setInfo("my-engine", "1.0");
//     s.advertise(rcp.Capability.Retrieve, { maxK: 100, modes: ["hybrid"] });
//     s.on("retrieve", (params) => ({ hits: myIndex.search(params.query, params.k ?? 10) }));
//     await s.serveStdio();

export {
  Capability,
  Errc,
  Method,
  PROTOCOL_VERSION,
  RcpError,
  negotiateVersion,
} from "./types.js";
export { Client } from "./client.js";
export { Server, makeLogNotification, makeProgressNotification } from "./server.js";
export { EngineSpec, Selector } from "./selector.js";
export { HttpTransport, StdioTransport } from "./transport.js";

import { Client } from "./client.js";

export const VERSION = "1.0.0";

// Spawn an RCP server subprocess and connect. `argv` e.g. ["node", "srv.js"].
export async function connectStdio(argv) {
  const c = new Client();
  await c.connectStdio(Array.from(argv));
  return c;
}

// Connect to an HTTP RCP server, e.g. "http://127.0.0.1:8000/rcp".
export async function connectHttp(baseUrl) {
  const c = new Client();
  await c.connectHttp(baseUrl);
  return c;
}
