// server.js — expose a Node RAG engine as an RCP/1 server.
//
// Mirrors the C++/Python `Server`: it owns the JSON-RPC framing, the
// `initialize` handshake, capability gating, and error mapping; you provide
// method handlers (`on`) and identity/capabilities (`setInfo` / `advertise`).
//
// Dispatch rules replicated exactly:
//   * initialize / info / ping / shutdown / notifications/cancel are ungated and
//     answer before initialize.
//   * A gated method before initialize -> -32001 NotInitialized.
//   * A method whose capability is unadvertised or whose handler is unregistered
//     -> -32003 CapabilityMissing.
//   * An unknown method -> -32004 UnknownMethod. A malformed request -> -32600.
//   * Batches (JSON array) yield one response per request; notifications drop out.
//
// Handlers may be sync or async (they are awaited).

import http from "node:http";
import readline from "node:readline";
import {
  CAP_FOR_METHOD,
  Errc,
  MIN_PROTOCOL_VERSION,
  Method,
  PROTOCOL_VERSION,
  RcpError,
  negotiateVersion,
} from "./types.js";

const KNOWN_METHODS = new Set(Object.keys(CAP_FOR_METHOD));

function ok(id, result) {
  return { jsonrpc: "2.0", id, result };
}

function err(id, code, message, data = null) {
  const error = { code, message };
  if (data !== null && data !== undefined) error.data = data;
  return { jsonrpc: "2.0", id, error };
}

// Build a `notifications/log` frame (spec §17.1) for a server to emit on its
// output stream. `level` ∈ debug|info|notice|warning|error.
export function makeLogNotification(level, message, data = null) {
  const params = { level, message };
  if (data !== null && data !== undefined) params.data = data;
  return JSON.stringify({ jsonrpc: "2.0", method: Method.LOG, params });
}

// Build a `notifications/progress` frame (spec §9) during a long retrieve.
// `token` echoes the request's `_meta.progressToken`; `progress` is 0..1.
export function makeProgressNotification(token, progress, stage = "", partial = null) {
  const params = { progressToken: token, progress };
  if (stage) params.stage = stage;
  if (partial !== null && partial !== undefined) params.partial = partial;
  return JSON.stringify({ jsonrpc: "2.0", method: Method.PROGRESS, params });
}

export class Server {
  constructor() {
    this._info = { name: "unknown", version: "0" };
    this._caps = {}; // wire JSON key -> metadata object
    this._handlers = {}; // method string -> (params) => result | Promise<result>
    this._initialized = false;
    this._httpServer = null;
  }

  // ── configuration ───────────────────────────────────────────────────────
  setInfo(name, version) {
    this._info = { name: String(name), version: String(version) };
    return this;
  }

  // Advertise a capability with optional metadata (stored under its wire key).
  // Advertising is independent of registering a handler; a gated call needs both
  // or it is CapabilityMissing.
  advertise(capability, meta = null) {
    this._caps[capability] = meta ?? {};
    return this;
  }

  // Register a handler for `method` (a Method.* value or its string). The
  // handler takes the params object and returns the result (or a Promise of it).
  on(method, fn) {
    const name = String(method);
    if (!KNOWN_METHODS.has(name)) {
      throw new Error(`unknown method hook: '${name}' (expected one of ${[...KNOWN_METHODS].join(", ")})`);
    }
    if (typeof fn !== "function") throw new TypeError("handler must be a function");
    this._handlers[name] = fn;
    return this;
  }

  // ── dispatch ──────────────────────────────────────────────────────────────
  // Handle one JSON-RPC request object -> reply object, or `null` for a
  // notification that warrants no response. Never throws.
  async handle(request) {
    const id = request && typeof request === "object" && "id" in request ? request.id : null;
    if (!request || typeof request !== "object" || typeof request.method !== "string") {
      return err(id, Errc.INVALID_REQUEST, "missing 'method'");
    }
    const m = request.method;
    const params = "params" in request ? request.params : {};

    if (m === Method.INITIALIZE) {
      this._initialized = true;
      const peer = params && typeof params === "object" ? params.protocolVersion ?? PROTOCOL_VERSION : PROTOCOL_VERSION;
      const neg = negotiateVersion(peer);
      if (neg < MIN_PROTOCOL_VERSION) return err(id, Errc.VERSION_MISMATCH, "no common version");
      return ok(id, this._infoResult(neg));
    }
    if (m === Method.INFO) return ok(id, this._infoResult(PROTOCOL_VERSION));
    if (m === Method.PING) {
      const hasNonce = params && typeof params === "object" && "nonce" in params;
      return ok(id, hasNonce ? { nonce: params.nonce } : {});
    }
    if (m === Method.CANCEL) return null; // notification: never answered
    if (m === Method.SHUTDOWN) return ok(id, {});

    if (!this._initialized) return err(id, Errc.NOT_INITIALIZED, "call 'initialize' first");

    return this._dispatch(id, m, params && typeof params === "object" ? params : {});
  }

  async _dispatch(id, m, params) {
    const cap = CAP_FOR_METHOD[m];
    if (!cap) return err(id, Errc.UNKNOWN_METHOD, `unknown method '${m}'`);
    const fn = this._handlers[m];
    if (!fn) return err(id, Errc.CAPABILITY_MISSING, `'${m}' not implemented`);
    if (this._caps[cap] == null) return err(id, Errc.CAPABILITY_MISSING, `capability '${cap}' not supported`);
    try {
      const result = await fn(params);
      return ok(id, result ?? {});
    } catch (e) {
      if (e instanceof RcpError) return err(id, e.code, e.detail, e.data);
      return err(id, Errc.INTERNAL_ERROR, String(e && e.message ? e.message : e));
    }
  }

  // Parse a JSON line, dispatch (single or batch), and return the reply string
  // ("" when there is no response, e.g. an all-notification batch).
  async handleLine(line) {
    let msg;
    try {
      msg = JSON.parse(line);
    } catch (e) {
      return JSON.stringify(err(null, Errc.PARSE_ERROR, e.message));
    }
    if (Array.isArray(msg)) {
      if (msg.length === 0) return JSON.stringify(err(null, Errc.INVALID_REQUEST, "empty batch"));
      const out = [];
      for (const el of msg) {
        const reply = await this.handle(el);
        if (reply !== null && reply !== undefined) out.push(reply);
      }
      return out.length ? JSON.stringify(out) : "";
    }
    const reply = await this.handle(msg);
    return reply === null || reply === undefined ? "" : JSON.stringify(reply);
  }

  _infoResult(version) {
    return {
      protocolVersion: version,
      server: { ...this._info },
      capabilities: { ...this._caps },
    };
  }

  // ── serving ──────────────────────────────────────────────────────────────
  // Serve newline-delimited JSON-RPC over stdin/stdout until shutdown/EOF. The
  // `for await` loop processes one line at a time, so async handlers cannot
  // interleave or reorder replies.
  async serveStdio() {
    const rl = readline.createInterface({ input: process.stdin, crlfDelay: Infinity });
    for await (const line of rl) {
      if (!line.trim()) continue;
      let isShutdown = false;
      try {
        const p = JSON.parse(line);
        isShutdown = p && typeof p === "object" && p.method === Method.SHUTDOWN;
      } catch {
        // fall through: handleLine will emit the ParseError reply
      }
      const reply = await this.handleLine(line);
      if (reply) {
        try {
          process.stdout.write(reply + "\n");
        } catch {
          break; // peer gone
        }
      }
      if (isShutdown) break;
    }
    rl.close();
  }

  // Serve JSON-RPC over minimal HTTP/1.1 on 127.0.0.1:port. A response returns
  // 200; a notification (no reply) returns 204. Resolves only when closed.
  serveHttp(port) {
    return new Promise((resolve, reject) => {
      const server = http.createServer((req, res) => {
        const chunks = [];
        req.on("data", (c) => chunks.push(c));
        req.on("end", async () => {
          let text = Buffer.concat(chunks).toString("utf8");
          if (!text.trim()) text = "{}";
          let reply;
          try {
            reply = await this.handleLine(text);
          } catch (e) {
            reply = JSON.stringify(err(null, Errc.INTERNAL_ERROR, String(e && e.message ? e.message : e)));
          }
          if (reply) {
            const payload = Buffer.from(reply, "utf8");
            res.writeHead(200, {
              "Content-Type": "application/json",
              "Content-Length": payload.length,
              Connection: "close",
            });
            res.end(payload);
          } else {
            res.writeHead(204, { Connection: "close" });
            res.end();
          }
        });
        req.on("error", () => {
          try {
            res.writeHead(400);
            res.end();
          } catch {}
        });
      });
      this._httpServer = server;
      server.on("error", (e) => reject(new RcpError(Errc.BACKEND_UNAVAILABLE, `listen failed: ${e.message}`)));
      server.on("close", () => resolve());
      server.listen(port, "127.0.0.1");
    });
  }

  // Stop an HTTP server started with serveHttp().
  close() {
    if (this._httpServer) {
      this._httpServer.close();
      this._httpServer = null;
    }
  }
}
