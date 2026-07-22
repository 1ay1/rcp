// selector.js — choose ONE backend from several reachable engines.
//
// Mirrors the C++/Python `Selector`: list candidate engines (by hand or from an
// rcp.json registry) and pick one — by id, by required capability, or by
// priority with liveness fallback. Connection is lazy: an engine is dialled only
// when actually selected. The result is a fully-connected `Client`.

import { readFileSync } from "node:fs";
import { Client } from "./client.js";
import { Errc, RcpError } from "./types.js";

// How to reach one backend. Inert until selected; exactly one of `argv` (stdio
// subprocess) or `url` (HTTP) is set.
export class EngineSpec {
  constructor({ id, argv = [], url = "", priority = 0, weight = 1.0, required = false }) {
    this.id = id;
    this.argv = Array.from(argv);
    this.url = url;
    this.priority = priority;
    this.weight = weight;
    this.required = required;
  }

  async open() {
    const c = new Client();
    if (this.argv.length) return c.connectStdio(this.argv);
    if (this.url) return c.connectHttp(this.url);
    throw new RcpError(Errc.INVALID_PARAMS, `engine '${this.id}' has no transport`);
  }

  // Parse one registry entry (spec §16.1). `transport` is "stdio" (needs
  // `command`) or "http" (needs `url`); inferred if omitted.
  static fromJson(entry) {
    if (!entry || typeof entry !== "object" || !("id" in entry)) {
      throw new RcpError(Errc.INVALID_PARAMS, "registry entry needs an 'id'");
    }
    const transport = entry.transport || "";
    const command = entry.command || entry.argv;
    const url = entry.url || "";
    const spec = new EngineSpec({
      id: entry.id ?? "",
      priority: entry.priority ?? 0,
      weight: entry.weight ?? 1.0,
      required: entry.required ?? false,
    });
    if (transport === "stdio" || (!transport && command)) {
      if (!Array.isArray(command) || command.length === 0) {
        throw new RcpError(Errc.INVALID_PARAMS, `stdio engine '${spec.id}' needs a 'command' array`);
      }
      spec.argv = command.map(String);
    } else if (transport === "http" || (!transport && url)) {
      if (!url) throw new RcpError(Errc.INVALID_PARAMS, `http engine '${spec.id}' needs a 'url'`);
      spec.url = url;
    } else {
      throw new RcpError(Errc.INVALID_PARAMS, `engine '${spec.id}' has no usable transport`);
    }
    return spec;
  }
}

export class Selector {
  constructor() {
    this._specs = [];
  }

  // ── construction ──────────────────────────────────────────────────────────
  static load(path) {
    return Selector.loads(readFileSync(path, "utf8"));
  }

  static loads(text) {
    let doc;
    try {
      doc = JSON.parse(text);
    } catch (e) {
      throw new RcpError(Errc.PARSE_ERROR, `invalid registry JSON: ${e.message}`);
    }
    const engines = doc && typeof doc === "object" ? doc.engines || [] : [];
    const sel = new Selector();
    for (const entry of engines) sel._specs.push(EngineSpec.fromJson(entry));
    return sel;
  }

  addStdio(id, argv, priority = 0) {
    this._specs.push(new EngineSpec({ id, argv: Array.from(argv), priority }));
    return this;
  }

  addHttp(id, url, priority = 0) {
    this._specs.push(new EngineSpec({ id, url, priority }));
    return this;
  }

  get size() {
    return this._specs.length;
  }

  // ── selection ─────────────────────────────────────────────────────────────
  // Connect the engine with the given id (or throw if unknown).
  async select(id) {
    for (const spec of this._specs) if (spec.id === id) return spec.open();
    throw new RcpError(Errc.INVALID_PARAMS, `no engine with id '${id}'`);
  }

  // Connect the highest-priority engine that answers (liveness fallback).
  async selectPrimary() {
    let last = null;
    for (const spec of this._byPriority()) {
      try {
        return await spec.open();
      } catch (e) {
        last = e;
      }
    }
    throw last || new RcpError(Errc.BACKEND_UNAVAILABLE, "no engines configured");
  }

  // Connect the highest-priority engine that answers AND advertises `capability`.
  async selectCapable(capability) {
    let last = null;
    for (const spec of this._byPriority()) {
      let c;
      try {
        c = await spec.open();
      } catch (e) {
        last = e;
        continue;
      }
      if (c.supports(capability)) return c;
      await c.shutdown();
      last = new RcpError(Errc.CAPABILITY_MISSING, `engine '${spec.id}' lacks '${capability}'`);
    }
    throw last || new RcpError(Errc.BACKEND_UNAVAILABLE, "no capable engine");
  }

  // Stable sort by descending priority (Array.prototype.sort is stable).
  _byPriority() {
    return [...this._specs].sort((a, b) => b.priority - a.priority);
  }
}
