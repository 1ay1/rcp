// transport.js — the request/reply seam (subprocess pipe or HTTP/1.1).
//
// A transport is `async call(request) -> reply`. Two concrete transports mirror
// the C++/Python SDKs:
//
//   * StdioTransport — newline-delimited JSON over a child's stdin/stdout.
//   * HttpTransport  — minimal HTTP/1.1 POST to `<base>/<method>`.
//
// Failures throw RcpError with the same typed codes as the other SDKs.

import { spawn } from "node:child_process";
import http from "node:http";
import { Errc, RcpError } from "./types.js";

// Newline-delimited JSON over a subprocess' stdio. Replies are correlated to
// requests by JSON-RPC id; server-initiated notifications (frames carrying
// `method`) and stray mismatched-id replies are skipped (spec §4.7).
export class StdioTransport {
  constructor(child) {
    this._child = child;
    this._buf = "";
    this._pending = new Map(); // id -> { resolve, reject }
    this._closed = false;

    child.stdout.setEncoding("utf8");
    child.stdout.on("data", (chunk) => this._onData(chunk));
    child.stdout.on("close", () => this._fail("server closed the connection"));
    child.on("exit", () => this._fail("server exited"));
    child.on("error", (e) => this._fail(`server process error: ${e.message}`));
    child.stdin.on("error", () => {}); // EPIPE surfaces via the write callback
  }

  static spawn(argv) {
    if (!Array.isArray(argv) || argv.length === 0) {
      throw new RcpError(Errc.INVALID_PARAMS, "server command must be a non-empty array");
    }
    let child;
    try {
      child = spawn(argv[0], argv.slice(1), { stdio: ["pipe", "pipe", "inherit"] });
    } catch (e) {
      throw new RcpError(Errc.BACKEND_UNAVAILABLE, `cannot spawn ${argv[0]}: ${e.message}`);
    }
    return new StdioTransport(child);
  }

  call(request) {
    return new Promise((resolve, reject) => {
      if (this._closed) {
        reject(new RcpError(Errc.BACKEND_UNAVAILABLE, "transport is closed"));
        return;
      }
      const id = request.id;
      this._pending.set(id, { resolve, reject });
      this._child.stdin.write(JSON.stringify(request) + "\n", (err) => {
        if (err) {
          this._pending.delete(id);
          reject(new RcpError(Errc.BACKEND_UNAVAILABLE, "write to server failed"));
        }
      });
    });
  }

  _onData(chunk) {
    this._buf += chunk;
    let nl;
    while ((nl = this._buf.indexOf("\n")) >= 0) {
      const line = this._buf.slice(0, nl);
      this._buf = this._buf.slice(nl + 1);
      if (!line.trim()) continue;
      let reply;
      try {
        reply = JSON.parse(line);
      } catch {
        continue; // skip a line we can't parse rather than derail the stream
      }
      if (reply && typeof reply === "object" && "method" in reply) continue; // notification
      const waiter = this._pending.get(reply?.id);
      if (waiter) {
        this._pending.delete(reply.id);
        waiter.resolve(reply);
      }
      // else: a stray reply with an unknown id — ignore.
    }
  }

  _fail(message) {
    if (this._closed) return;
    this._closed = true;
    const err = new RcpError(Errc.BACKEND_UNAVAILABLE, message);
    for (const { reject } of this._pending.values()) reject(err);
    this._pending.clear();
  }

  close() {
    this._closed = true;
    try { this._child.stdin.end(); } catch {}
    try { this._child.kill("SIGTERM"); } catch {}
  }
}

// Minimal HTTP/1.1 client: POST the request to `<baseUrl>/<method>`. The server
// ignores the path and dispatches on the body's `method`; the path is a
// human-readable convenience. Maps HTTP 429 -> RateLimited and 503 ->
// BackendUnavailable (spec §5.2).
export class HttpTransport {
  constructor(baseUrl, timeoutMs = 30000) {
    this._base = String(baseUrl).replace(/\/+$/, "");
    this._timeout = timeoutMs;
  }

  call(request) {
    return new Promise((resolve, reject) => {
      const method = request.method || "";
      let url;
      try {
        url = new URL(this._base + "/" + method);
      } catch (e) {
        reject(new RcpError(Errc.INVALID_PARAMS, `bad URL: ${e.message}`));
        return;
      }
      if (url.protocol !== "http:") {
        reject(new RcpError(Errc.INVALID_PARAMS, `unsupported URL scheme '${url.protocol}'`));
        return;
      }
      const body = Buffer.from(JSON.stringify(request), "utf8");
      const req = http.request(
        {
          hostname: url.hostname,
          port: url.port || 80,
          path: url.pathname + url.search,
          method: "POST",
          headers: { "Content-Type": "application/json", "Content-Length": body.length },
          timeout: this._timeout,
        },
        (res) => {
          const chunks = [];
          res.on("data", (c) => chunks.push(c));
          res.on("end", () => {
            const status = res.statusCode;
            const data = Buffer.concat(chunks);
            if (status === 429) { reject(new RcpError(Errc.RATE_LIMITED, "HTTP 429")); return; }
            if (status === 503) { reject(new RcpError(Errc.BACKEND_UNAVAILABLE, "HTTP 503")); return; }
            if ((status < 200 || status >= 300) && data.toString("utf8").trim() === "") {
              reject(new RcpError(Errc.BACKEND_UNAVAILABLE, `HTTP ${status}`));
              return;
            }
            try {
              resolve(JSON.parse(data.toString("utf8")));
            } catch (e) {
              reject(new RcpError(Errc.PARSE_ERROR, e.message));
            }
          });
        }
      );
      req.on("error", (e) => reject(new RcpError(Errc.BACKEND_UNAVAILABLE, `HTTP request failed: ${e.message}`)));
      req.on("timeout", () => req.destroy(new RcpError(Errc.BACKEND_UNAVAILABLE, "HTTP request timed out")));
      req.write(body);
      req.end();
    });
  }

  close() {}
}
