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

    def close(self):
        pass


def run(t):
    print("RCP/1 conformance suite\n")

    # info before initialize MUST work
    r = t.raw({"jsonrpc": "2.0", "id": 0, "method": Method.INFO, "params": {}})
    check("info answers before initialize", r.get("result", {}).get("server") is not None)

    # method before initialize MUST be NotInitialized
    r = t.raw({"jsonrpc": "2.0", "id": 1, "method": Method.RETRIEVE, "params": {"query": "x", "k": 1}})
    check("pre-initialize call rejected with -32001",
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
        check("retrieve returns a 'hits' array",
              isinstance(r.get("result", {}).get("hits"), list), str(r))

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
