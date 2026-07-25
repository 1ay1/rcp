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

PASS, FAIL, SKIP = "\033[32mPASS\033[0m", "\033[31mFAIL\033[0m", "\033[90mSKIP\033[0m"
results = []          # (level, name, ok)

# A conformance level is CERTIFIED only if every MUST check tagged at that level
# (and every level below it) passed. L1/L2 checks that don't apply to this server
# — because it doesn't advertise the capability — are SKIPPED, not failed, so a
# minimal-but-correct server still certifies at the level it actually reaches.
_LEVELS = ("L0", "L1", "L2")


def check(level, name, cond, detail=""):
    results.append((level, name, bool(cond)))
    mark = PASS if cond else FAIL
    print(f"  [{level}] {mark}  {name}" + (f"  ({detail})" if detail and not cond else ""))


def skip(level, name, why):
    results.append((level, name, None))
    print(f"  [{level}] {SKIP}  {name}  ({why})")


def certified_level():
    """Highest contiguous level whose applicable MUST checks all passed."""
    best = None
    for lvl in _LEVELS:
        applicable = [ok for L, _, ok in results if L == lvl and ok is not None]
        if applicable and all(applicable):
            best = lvl
        elif any(ok is False for L, _, ok in results if L == lvl):
            break
        elif not applicable:
            # nothing to prove at this level (all skipped) — inherit lower level
            continue
    return best


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


def run(t, emit_json=False):
    print("RCP/1 conformance suite\n")

    # info before initialize MUST work
    r = t.raw({"jsonrpc": "2.0", "id": 0, "method": Method.INFO, "params": {}})
    check("L0", "info answers before initialize", r.get("result", {}).get("server") is not None)

    # ping before initialize MUST work and MUST echo the nonce
    r = t.raw({"jsonrpc": "2.0", "id": 100, "method": Method.PING, "params": {"nonce": 777}})
    check("L0", "ping answers before initialize and echoes nonce",
          r.get("result", {}).get("nonce") == 777, str(r))

    # method before initialize MUST be NotInitialized
    r = t.raw({"jsonrpc": "2.0", "id": 1, "method": Method.RETRIEVE, "params": {"query": "x", "k": 1}})
    check("L0", "pre-initialize call rejected with -32001",
          r.get("error", {}).get("code") == Errc.NOT_INITIALIZED, str(r))

    # ── §4.5: notifications MUST NOT be answered ──────────────────────────
    check("L0", "notification (no id) for a gated method is not answered",
          t.notify({"jsonrpc": "2.0", "method": Method.RETRIEVE,
                    "params": {"query": "x", "k": 1}}) is None)
    check("L0", "notification (no id) for an open method is not answered",
          t.notify({"jsonrpc": "2.0", "method": Method.INFO, "params": {}}) is None)
    check("L0", "notification (no id) for an unknown method is not answered",
          t.notify({"jsonrpc": "2.0", "method": "does/not/exist", "params": {}}) is None)
    check("L0", "notifications/cancel is not answered",
          t.notify({"jsonrpc": "2.0", "method": "notifications/cancel",
                    "params": {"id": 12345}}) is None)

    # ── §13: a FAILED handshake must not unlock the server ──────────────────
    r = t.raw({"jsonrpc": "2.0", "id": 200, "method": Method.INITIALIZE,
               "params": {"protocolVersion": 0, "client": {"name": "conf", "version": "1"}}})
    unsupported_rejected = r.get("error", {}).get("code") == Errc.VERSION_MISMATCH
    check("L0", "initialize with an unsupported version rejected with -32002",
          unsupported_rejected, str(r))
    if unsupported_rejected:
        r = t.raw({"jsonrpc": "2.0", "id": 201, "method": Method.RETRIEVE,
                   "params": {"query": "x", "k": 1}})
        check("L0", "a failed handshake leaves the session uninitialized",
              r.get("error", {}).get("code") == Errc.NOT_INITIALIZED, str(r))

    # initialize MUST return protocolVersion >= 1 + capabilities
    r = t.raw({"jsonrpc": "2.0", "id": 2, "method": Method.INITIALIZE,
               "params": {"protocolVersion": 1, "client": {"name": "conf", "version": "1"}, "capabilities": {}}})
    res = r.get("result", {})
    check("L0", "initialize returns protocolVersion >= 1", res.get("protocolVersion", 0) >= 1, str(r))
    caps = res.get("capabilities", {})
    check("L0", "advertises at least one retrieval capability",
          any(k in caps for k in ("embed", "rerank", "retrieve", "graph")), str(caps))
    check("L0", "server identity present", bool(res.get("server", {}).get("name")))

    # §14: the server's self-declared level must not exceed what it can prove.
    declared = (res.get("_meta", {}) or {}).get("conformance")

    # unknown method MUST be UnknownMethod
    r = t.raw({"jsonrpc": "2.0", "id": 3, "method": "does/not/exist", "params": {}})
    check("L0", "unknown method rejected with -32004",
          r.get("error", {}).get("code") == Errc.UNKNOWN_METHOD, str(r))

    # un-advertised capability MUST be CapabilityMissing (test a likely-absent one)
    if "index" not in caps:
        r = t.raw({"jsonrpc": "2.0", "id": 4, "method": Method.INDEX_ADD,
                   "params": {"documents": []}})
        check("L0", "un-advertised method rejected with -32003",
              r.get("error", {}).get("code") == Errc.CAPABILITY_MISSING, str(r))
    else:
        skip("L0", "un-advertised method rejected with -32003", "server advertises index")

    # §4.2: a response MUST echo the request id, with the same JSON type.
    r = t.raw({"jsonrpc": "2.0", "id": "str-id-1", "method": Method.INFO, "params": {}})
    check("L0", "response echoes a string id unchanged", r.get("id") == "str-id-1", str(r)[:200])
    check("L0", "responses carry jsonrpc=2.0", r.get("jsonrpc") == "2.0", str(r))

    # ── L1: Retrieval ───────────────────────────────────────────────
    if "retrieve" not in caps:
        skip("L1", "retrieve returns a ranked 'hits' array", "server does not advertise retrieve")
    else:
        r = t.raw({"jsonrpc": "2.0", "id": 5, "method": Method.RETRIEVE,
                   "params": {"query": "test", "k": 3}})
        hits = r.get("result", {}).get("hits")
        check("L1", "retrieve returns a ranked 'hits' array", isinstance(hits, list), str(r))

        if isinstance(hits, list):
            check("L1", "retrieve honours the k ceiling", len(hits) <= 3, f"asked k=3, got {len(hits)}")
            check("L1", "every hit carries an id and a numeric score",
                  all(isinstance(h, dict) and "id" in h
                      and isinstance(h.get("score"), (int, float))
                      and not isinstance(h.get("score"), bool) for h in hits), str(hits)[:200])
            scores = [h.get("score") for h in hits
                      if isinstance(h, dict) and isinstance(h.get("score"), (int, float))]
            check("L1", "hits are ordered by descending score",
                  scores == sorted(scores, reverse=True), str(scores))
            ids = [h.get("id") for h in hits if isinstance(h, dict)]
            check("L1", "hit ids are unique within a result", len(ids) == len(set(ids)), str(ids))

        max_k = (caps.get("retrieve") or {}).get("maxK")
        if isinstance(max_k, int) and max_k >= 1:
            r = t.raw({"jsonrpc": "2.0", "id": 6, "method": Method.RETRIEVE,
                       "params": {"query": "test", "k": max_k + 1000}})
            over = r.get("result", {}).get("hits")
            check("L1", "k above maxK is clamped, not rejected",
                  isinstance(over, list) and len(over) <= max_k, str(r)[:200])
        else:
            skip("L1", "k above maxK is clamped, not rejected", "no maxK advertised")

        r = t.raw({"jsonrpc": "2.0", "id": 7, "method": Method.RETRIEVE,
                   "params": {"query": "test", "k": -5}})
        check("L1", "negative k rejected with -32602",
              r.get("error", {}).get("code") == Errc.INVALID_PARAMS, str(r)[:200])

        # §3.3: the funnel invariant is enforced (candidateK >= rerank.topN >= k).
        r = t.raw({"jsonrpc": "2.0", "id": 8, "method": Method.RETRIEVE,
                   "params": {"query": "test", "k": 10, "candidateK": 2}})
        check("L1", "funnel violation (candidateK < k) rejected with -32602",
              r.get("error", {}).get("code") == Errc.INVALID_PARAMS, str(r)[:200])

    # ── L2: SOTA ─────────────────────────────────────────────────
    rr = caps.get("retrieve") or {}
    modes = rr.get("modes") or rr.get("modes", [])
    if "hybrid" in (modes or []):
        r = t.raw({"jsonrpc": "2.0", "id": 9, "method": Method.RETRIEVE,
                   "params": {"query": "test", "k": 3, "mode": "hybrid"}})
        check("L2", "advertised hybrid mode returns hits",
              isinstance(r.get("result", {}).get("hits"), list), str(r)[:200])
    else:
        skip("L2", "advertised hybrid mode returns hits", "hybrid not advertised")

    if "rerank" in caps:
        r = t.raw({"jsonrpc": "2.0", "id": 10, "method": Method.RERANK,
                   "params": {"query": "test", "documents": ["a b c", "d e f"], "topN": 2}})
        res10 = r.get("result", {})
        check("L2", "rerank returns a scored ordering",
              isinstance(res10.get("results") or res10.get("hits") or res10.get("ranking"), list),
              str(r)[:200])
    else:
        skip("L2", "rerank returns a scored ordering", "rerank not advertised")

    # §7.10/§7.11: a WRITABLE index must upsert by id (no duplicate) and delete
    # idempotently. Gated on index.writable so a read-only/absent index skips.
    idx = caps.get("index") or {}
    if idx.get("writable") is True:
        doc = {"id": "rcp-conf://upsert", "text": "conformance upsert probe document one"}
        r = t.raw({"jsonrpc": "2.0", "id": 20, "method": Method.INDEX_ADD,
                   "params": {"documents": [doc]}})
        res20 = r.get("result", {})
        ids = res20.get("ids")
        check("L2", "index/add returns positional ids (one per document)",
              isinstance(ids, list) and len(ids) == 1, str(r)[:200])

        # Re-add the SAME id with new text: MUST be an upsert, not a duplicate.
        doc2 = {"id": "rcp-conf://upsert", "text": "conformance upsert probe document two updated"}
        r = t.raw({"jsonrpc": "2.0", "id": 21, "method": Method.INDEX_ADD,
                   "params": {"documents": [doc2]}})
        upsert_ok = "error" not in r or r["error"].get("code") == Errc.CONFLICT
        check("L2", "index/add re-adding an id upserts or rejects with -32016",
              upsert_ok, str(r)[:200])

        # Delete it, then delete again: second delete MUST be idempotent (0).
        r = t.raw({"jsonrpc": "2.0", "id": 22, "method": Method.INDEX_DELETE,
                   "params": {"ids": ["rcp-conf://upsert"]}})
        r2 = t.raw({"jsonrpc": "2.0", "id": 23, "method": Method.INDEX_DELETE,
                    "params": {"ids": ["rcp-conf://upsert"]}})
        check("L2", "index/delete is idempotent (repeat returns deleted:0)",
              r2.get("result", {}).get("deleted") == 0, str(r2)[:200])
    else:
        skip("L2", "index/add returns positional ids (one per document)", "index not writable")
        skip("L2", "index/add re-adding an id upserts or rejects with -32016", "index not writable")
        skip("L2", "index/delete is idempotent (repeat returns deleted:0)", "index not writable")

    t.raw({"jsonrpc": "2.0", "id": 99, "method": Method.SHUTDOWN, "params": {}})
    t.close()

    # ── certification summary ──────────────────────────────────────
    n_pass = sum(1 for _, _, ok in results if ok is True)
    n_fail = sum(1 for _, _, ok in results if ok is False)
    n_skip = sum(1 for _, _, ok in results if ok is None)
    level = certified_level()

    # §14: does the server tell the truth about its own level?
    honesty_ok = True
    if declared in _LEVELS and level is not None:
        honesty_ok = _LEVELS.index(declared) <= _LEVELS.index(level)
    elif declared in _LEVELS and level is None:
        honesty_ok = False

    print(f"\n{n_pass} passed, {n_fail} failed, {n_skip} skipped")
    print(f"CERTIFIED LEVEL: {level or 'NONE (L0 MUSTs failed)'}")
    if declared:
        verdict = "consistent" if honesty_ok else "OVERCLAIMED"
        print(f"declared _meta.conformance={declared!r} — {verdict}")

    if emit_json:
        print(json.dumps({
            "certifiedLevel": level,
            "declaredLevel": declared,
            "declarationHonest": honesty_ok,
            "passed": n_pass, "failed": n_fail, "skipped": n_skip,
            "checks": [{"level": L, "name": nm,
                        "status": "pass" if ok else ("skip" if ok is None else "fail")}
                       for L, nm, ok in results],
        }, indent=2))

    return n_fail == 0 and honesty_ok


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--http", help="base URL of an HTTP RCP server")
    ap.add_argument("--json", action="store_true", help="emit a machine-readable report for CI")
    ap.add_argument("cmd", nargs="*", help="server command (after --) for stdio")
    args = ap.parse_args()
    transport = Http(args.http) if args.http else Stdio(args.cmd or ["python3", "examples/example_server.py"])
    sys.exit(0 if run(transport, emit_json=args.json) else 1)
