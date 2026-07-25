#!/usr/bin/env python3
"""RCP/1 conformance checker.

Runs a candidate server through the mandatory behaviours of the spec (§9) and
prints a pass/fail report. Usage:

    python3 conformance/check.py -- python3 examples/example_server.py
    python3 conformance/check.py --http http://127.0.0.1:8000/rcp

Exit code 0 iff all MUST checks pass.
"""
import argparse
import json
import subprocess
import sys
import urllib.request

sys.path.insert(0, __file__.rsplit("/", 2)[0] + "/sdk/python")
from rcp import Errc, Method  # noqa: E402

PASS, FAIL = "\033[32mPASS\033[0m", "\033[31mFAIL\033[0m"
results = []


def check(name, cond, detail=""):
    results.append((name, bool(cond)))
    print(f"  {PASS if cond else FAIL}  {name}" + (f"  ({detail})" if detail and not cond else ""))


class Stdio:
    def __init__(self, argv):
        self.p = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  text=True, bufsize=1)

    def raw(self, obj):
        self.p.stdin.write(json.dumps(obj) + "\n")
        self.p.stdin.flush()
        return json.loads(self.p.stdout.readline())

    def notify(self, obj, timeout=0.5):
        """Send a frame and return the reply, or None if the server stayed silent.

        §4.5 requires a notification (a frame with no `id`) to go unanswered, so
        the only way to test it is to wait and prove nothing came back."""
        import select
        self.p.stdin.write(json.dumps(obj) + "\n")
        self.p.stdin.flush()
        if select.select([self.p.stdout], [], [], timeout)[0]:
            line = self.p.stdout.readline()
            return json.loads(line) if line.strip() else None
        return None

    def close(self):
        self.p.stdin.close()
        try:
            self.p.wait(1)
        except Exception:
            self.p.kill()


class Http:
    def __init__(self, base):
        self.base = base.rstrip("/")

    def raw(self, obj):
        req = urllib.request.Request(f"{self.base}/{obj.get('method','')}",
                                     data=json.dumps(obj).encode(),
                                     headers={"Content-Type": "application/json"}, method="POST")
        with urllib.request.urlopen(req) as r:
            return json.loads(r.read())

    def notify(self, obj, timeout=0.5):
        """§5.2: a notification over HTTP is answered with 204 (or an empty body),
        never a JSON-RPC response object. Returns None when correctly silent."""
        req = urllib.request.Request(f"{self.base}/{obj.get('method','')}",
                                     data=json.dumps(obj).encode(),
                                     headers={"Content-Type": "application/json"}, method="POST")
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                body = r.read()
                if r.status == 204 or not body.strip():
                    return None
                return json.loads(body)
        except Exception:
            return None

    def close(self):
        pass


def run(t):
    print("RCP/1 conformance suite\n")

    # info before initialize MUST work
    r = t.raw({"jsonrpc": "2.0", "id": 0, "method": Method.INFO, "params": {}})
    check("info answers before initialize", r.get("result", {}).get("server") is not None)

    # ping before initialize MUST work and MUST echo the nonce
    r = t.raw({"jsonrpc": "2.0", "id": 100, "method": Method.PING, "params": {"nonce": 777}})
    check("ping answers before initialize and echoes nonce",
          r.get("result", {}).get("nonce") == 777, str(r))

    # method before initialize MUST be NotInitialized
    r = t.raw({"jsonrpc": "2.0", "id": 1, "method": Method.RETRIEVE, "params": {"query": "x", "k": 1}})
    check("pre-initialize call rejected with -32001",
          r.get("error", {}).get("code") == Errc.NOT_INITIALIZED, str(r))

    # ── §4.5: notifications MUST NOT be answered ─────────────────────────────
    # A frame with no `id` gets no reply — not a result, and not an error. A
    # server that answers desynchronises any client that correlates by id.
    check("notification (no id) for a gated method is not answered",
          t.notify({"jsonrpc": "2.0", "method": Method.RETRIEVE,
                    "params": {"query": "x", "k": 1}}) is None)
    check("notification (no id) for an open method is not answered",
          t.notify({"jsonrpc": "2.0", "method": Method.INFO, "params": {}}) is None)
    check("notification (no id) for an unknown method is not answered",
          t.notify({"jsonrpc": "2.0", "method": "does/not/exist", "params": {}}) is None)
    check("notifications/cancel is not answered",
          t.notify({"jsonrpc": "2.0", "method": "notifications/cancel",
                    "params": {"id": 12345}}) is None)

    # ── §13: a FAILED handshake must not unlock the server ───────────────────
    # negotiate_version() floors at MIN_PROTOCOL_VERSION, so version 0 must be
    # rejected AND must leave the session uninitialized.
    r = t.raw({"jsonrpc": "2.0", "id": 200, "method": Method.INITIALIZE,
               "params": {"protocolVersion": 0, "client": {"name": "conf", "version": "1"}}})
    unsupported_rejected = r.get("error", {}).get("code") == Errc.VERSION_MISMATCH
    check("initialize with an unsupported version rejected with -32002",
          unsupported_rejected, str(r))
    if unsupported_rejected:
        r = t.raw({"jsonrpc": "2.0", "id": 201, "method": Method.RETRIEVE,
                   "params": {"query": "x", "k": 1}})
        check("a failed handshake leaves the session uninitialized",
              r.get("error", {}).get("code") == Errc.NOT_INITIALIZED, str(r))

    # initialize MUST return protocolVersion >= 1 + capabilities
    r = t.raw({"jsonrpc": "2.0", "id": 2, "method": Method.INITIALIZE,
               "params": {"protocolVersion": 1, "client": {"name": "conf", "version": "1"}, "capabilities": {}}})
    res = r.get("result", {})
    check("initialize returns protocolVersion >= 1", res.get("protocolVersion", 0) >= 1, str(r))
    caps = res.get("capabilities", {})
    check("advertises at least one retrieval capability",
          any(k in caps for k in ("embed", "rerank", "retrieve", "graph")), str(caps))
    check("server identity present", bool(res.get("server", {}).get("name")))

    # unknown method MUST be UnknownMethod
    r = t.raw({"jsonrpc": "2.0", "id": 3, "method": "does/not/exist", "params": {}})
    check("unknown method rejected with -32004",
          r.get("error", {}).get("code") == Errc.UNKNOWN_METHOD, str(r))

    # un-advertised capability MUST be CapabilityMissing (test a likely-absent one)
    if "index" not in caps:
        r = t.raw({"jsonrpc": "2.0", "id": 4, "method": Method.INDEX_ADD,
                   "params": {"documents": []}})
        check("un-advertised method rejected with -32003",
              r.get("error", {}).get("code") == Errc.CAPABILITY_MISSING, str(r))

    # advertised retrieve MUST return a hits array
    if "retrieve" in caps:
        r = t.raw({"jsonrpc": "2.0", "id": 5, "method": Method.RETRIEVE,
                   "params": {"query": "test", "k": 3}})
        hits = r.get("result", {}).get("hits")
        check("retrieve returns a 'hits' array", isinstance(hits, list), str(r))

        if isinstance(hits, list):
            # §7.7: the server MUST NOT return more than k hits.
            check("retrieve honours the k ceiling", len(hits) <= 3,
                  f"asked k=3, got {len(hits)}")
            # §7.7: every hit MUST carry an id and a numeric score.
            check("every hit carries an id and a numeric score",
                  all(isinstance(h, dict) and "id" in h
                      and isinstance(h.get("score"), (int, float))
                      and not isinstance(h.get("score"), bool) for h in hits), str(hits)[:200])
            # §7.7: hits MUST be ordered by descending score.
            scores = [h.get("score") for h in hits
                      if isinstance(h, dict) and isinstance(h.get("score"), (int, float))]
            check("hits are ordered by descending score",
                  scores == sorted(scores, reverse=True), str(scores))
            # §4.7: ids MUST be unique within one result set.
            ids = [h.get("id") for h in hits if isinstance(h, dict)]
            check("hit ids are unique within a result", len(ids) == len(set(ids)), str(ids))

        # §7.7: k above the advertised maxK is CLAMPED, never an error.
        max_k = (caps.get("retrieve") or {}).get("maxK")
        if isinstance(max_k, int) and max_k >= 1:
            r = t.raw({"jsonrpc": "2.0", "id": 6, "method": Method.RETRIEVE,
                       "params": {"query": "test", "k": max_k + 1000}})
            over = r.get("result", {}).get("hits")
            check("k above maxK is clamped, not rejected",
                  isinstance(over, list) and len(over) <= max_k, str(r)[:200])

        # §7.7: a structurally invalid k is -32602 (not a clamp).
        r = t.raw({"jsonrpc": "2.0", "id": 7, "method": Method.RETRIEVE,
                   "params": {"query": "test", "k": -5}})
        check("negative k rejected with -32602",
              r.get("error", {}).get("code") == Errc.INVALID_PARAMS, str(r)[:200])

    # §4.2: a response MUST echo the request id, with the same JSON type.
    r = t.raw({"jsonrpc": "2.0", "id": "str-id-1", "method": Method.INFO, "params": {}})
    check("response echoes a string id unchanged", r.get("id") == "str-id-1", str(r)[:200])

    # every reply MUST be valid JSON-RPC 2.0
    check("responses carry jsonrpc=2.0", r.get("jsonrpc") == "2.0", str(r))

    t.raw({"jsonrpc": "2.0", "id": 99, "method": Method.SHUTDOWN, "params": {}})
    t.close()

    n_pass = sum(1 for _, ok in results if ok)
    print(f"\n{n_pass}/{len(results)} checks passed")
    return all(ok for _, ok in results)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--http", help="base URL of an HTTP RCP server")
    ap.add_argument("cmd", nargs="*", help="server command (after --) for stdio")
    args = ap.parse_args()
    transport = Http(args.http) if args.http else Stdio(args.cmd or ["python3", "examples/example_server.py"])
    sys.exit(0 if run(transport) else 1)
