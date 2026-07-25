#!/usr/bin/env python3
"""example_streaming.py — an END-TO-END, runnable HTTP + SSE streaming demo.

RCP's progress story (spec §9 notifications + §13 SSE transport) is not just
prose here: this script starts a real HTTP server whose `retrieve` STREAMS its
pipeline — emitting `notifications/progress` frames for each stage (recall →
fuse → rerank) as they happen — then delivers the final response, all over one
`text/event-stream` connection. A tiny raw-socket client consumes the frames
live so you can watch the funnel fill in real time.

Run it directly; it spawns the server, streams one query, and exits:

    python3 examples/example_streaming.py

The same server also answers a plain (non-SSE) POST with a single buffered JSON
response — the streaming handler is written once and works both ways (spec §13:
"If the client did not request SSE, the server MUST return a single buffered
JSON response").
"""
from __future__ import annotations

import json
import socket
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "sdk" / "python"))

import rcp  # noqa: E402

# ── a tiny corpus + toy scorers, enough to show a real 3-stage funnel ─────────
DOCS = [
    {"id": "d1", "text": "The Eiffel Tower is an iron lattice tower in Paris."},
    {"id": "d2", "text": "Paris is the capital and most populous city of France."},
    {"id": "d3", "text": "The Louvre is the world's most-visited art museum."},
    {"id": "d4", "text": "Mount Fuji is the tallest mountain in Japan."},
    {"id": "d5", "text": "Tokyo is the capital of Japan and a global finance hub."},
]


def _overlap(query: str, text: str) -> float:
    q = set(query.lower().split())
    t = set(text.lower().split())
    return len(q & t) / (len(q) or 1)


def streaming_retrieve(params):
    """A generator retrieve: yield rcp.Progress per stage, then return the result.

    Honours the §3.3 funnel candidateK >= topN >= k, streams a progress frame
    (with a `partial` early-result peek) after each stage, and fuses the dense +
    sparse recall lists with the reference RRF from `rcp.rrf_fuse`.
    """
    query = params.get("query", "")
    k = params.get("k", 3)
    cand = params.get("candidateK", max(k * 2, 4))

    # Stage 1 — dense recall (toy: word overlap as a stand-in for cosine).
    time.sleep(0.15)
    dense = sorted(
        ({"id": d["id"], "text": d["text"], "score": _overlap(query, d["text"])} for d in DOCS),
        key=lambda h: -h["score"],
    )[:cand]
    yield rcp.Progress(0.33, "recall:dense", partial=[h["id"] for h in dense[:k]])

    # Stage 2 — sparse recall (toy: reversed lexical overlap for variety).
    time.sleep(0.15)
    sparse = sorted(
        ({"id": d["id"], "text": d["text"], "score": _overlap(query, d["text"]) * 0.9} for d in DOCS),
        key=lambda h: -h["score"],
    )[:cand]
    fused = rcp.rrf_fuse({"dense": dense, "sparse": sparse}, k=cand)
    yield rcp.Progress(0.66, "fuse:rrf", partial=[h["id"] for h in fused[:k]])

    # Stage 3 — rerank (toy: re-sort by exact overlap, take top-k).
    time.sleep(0.15)
    reranked = sorted(fused, key=lambda h: -_overlap(query, h.get("text", "")))[:k]
    yield rcp.Progress(1.0, "rerank", partial=[h["id"] for h in reranked])

    return {
        "hits": reranked,
        "usage": {"candidateK": cand, "topN": len(fused), "returned": len(reranked)},
        "mode": "hybrid",
    }


def build_server() -> rcp.Server:
    s = rcp.Server()
    s.set_info("streaming-demo", "1.0")
    s.advertise(rcp.Capability.Retrieve, {"maxK": 100, "modes": ["hybrid"]})
    s.advertise(rcp.Capability.Streaming)  # §13: required for the SSE path
    s.stream("retrieve", streaming_retrieve)
    return s


# ── a minimal SSE client: POST with Accept: text/event-stream, print frames ───
def sse_retrieve(host: str, port: int, query: str, k: int = 3):
    req = {
        "jsonrpc": "2.0",
        "id": 1,
        "method": "retrieve",
        "params": {"query": query, "k": k, "candidateK": 4, "_meta": {"progressToken": "t-1"}},
    }
    body = json.dumps(req).encode("utf-8")
    head = (
        f"POST /rcp HTTP/1.1\r\nHost: {host}:{port}\r\n"
        "Accept: text/event-stream\r\nContent-Type: application/json\r\n"
        f"Content-Length: {len(body)}\r\nConnection: close\r\n\r\n"
    ).encode("utf-8")

    sock = socket.create_connection((host, port), timeout=10)
    sock.sendall(head + body)

    raw = b""
    while b"\r\n\r\n" not in raw:
        raw += sock.recv(4096)
    _, _, rest = raw.partition(b"\r\n\r\n")

    buf = rest
    final = None
    while True:
        # SSE frames are separated by a blank line; each is a `data: <json>` line.
        while b"\n\n" not in buf:
            chunk = sock.recv(4096)
            if not chunk:
                break
            buf += chunk
        if b"\n\n" not in buf:
            break
        frame, _, buf = buf.partition(b"\n\n")
        for line in frame.split(b"\n"):
            if not line.startswith(b"data:"):
                continue
            msg = json.loads(line[len(b"data:"):].strip())
            if msg.get("method") == "notifications/progress":
                p = msg["params"]
                print(f"  progress {p['progress']:>4.0%}  {p.get('stage',''):<14} "
                      f"partial={p.get('partial')}")
            else:  # the final response frame
                final = msg
    sock.close()
    return final


def main() -> int:
    server = build_server()
    # HTTP is request-scoped (spec §13); prime the handshake once in-process so
    # the shared server instance treats subsequent HTTP requests as initialized.
    server.handle({"jsonrpc": "2.0", "id": 0, "method": "initialize",
                   "params": {"protocolVersion": 1}})
    port = 8807
    t = threading.Thread(target=server.serve_http, args=(port,), daemon=True)
    t.start()
    time.sleep(0.3)  # let the listener bind

    print("streaming retrieve for 'capital city of France' (watch the funnel fill):")
    final = sse_retrieve("127.0.0.1", port, "capital city of France", k=3)

    assert final is not None and "result" in final, final
    hits = final["result"]["hits"]
    print("\nfinal response frame:")
    print(f"  hits    = {[h['id'] for h in hits]}")
    print(f"  usage   = {final['result']['usage']}")
    print(f"  mode    = {final['result']['mode']}")

    # The identical handler also answers a plain unary request (no SSE); the
    # server was already initialized above.
    assert server._info_result(1)["capabilities"].get("streaming") is not None
    reply = server.handle({
        "jsonrpc": "2.0", "id": 3, "method": "retrieve",
        "params": {"query": "capital city of France", "k": 2, "candidateK": 4},
    })
    print("\nsame handler, plain unary (no SSE):")
    print(f"  hits    = {[h['id'] for h in reply['result']['hits']]}")

    print("\nOK — SSE progress frames + final response delivered over one stream.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
