"""RCP server — expose a Python RAG engine as an RCP/1 server.

Mirrors the C++ ``rcp::Server`` (``sdk/cpp/include/rcp/server.hpp``): it owns the
JSON-RPC framing, the ``initialize`` handshake, capability gating, and error
mapping; you provide method handlers (``on``) and identity/capabilities
(``set_info`` / ``advertise``).

Dispatch rules replicated exactly:

* ``initialize`` / ``info`` / ``ping`` / ``shutdown`` / ``notifications/cancel``
  are ungated and answer before initialize.
* A gated method before ``initialize`` → **-32001** NotInitialized.
* A method whose capability is unadvertised or whose handler is unregistered →
  **-32003** CapabilityMissing.
* An unknown method → **-32004** UnknownMethod. A malformed request → **-32600**.
* Batches (JSON array) yield one response per request; notifications drop out.
"""
from __future__ import annotations

import json
import socket

from ._types import (
    CAP_FOR_METHOD,
    MIN_PROTOCOL_VERSION,
    PROTOCOL_VERSION,
    Capability,
    Errc,
    Method,
    RcpError,
    cap_key,
    negotiate_version,
)

_COMPACT = (",", ":")
_KNOWN_METHODS = set(CAP_FOR_METHOD)


def _dumps(obj) -> str:
    return json.dumps(obj, separators=_COMPACT)


def _ok(id_, result) -> dict:
    return {"jsonrpc": "2.0", "id": id_, "result": result}


def _err(id_, code: int, message: str, data=None) -> dict:
    error = {"code": code, "message": message}
    if data is not None:
        error["data"] = data
    return {"jsonrpc": "2.0", "id": id_, "error": error}


def make_log_notification(level: str, message: str, data=None) -> str:
    """Build a ``notifications/log`` frame (spec §17.1) for a server to emit on
    its output stream. ``level`` ∈ debug|info|notice|warning|error."""
    params = {"level": level, "message": message}
    if data is not None:
        params["data"] = data
    return _dumps({"jsonrpc": "2.0", "method": Method.LOG, "params": params})


class Progress:
    """A streaming progress event yielded by a :meth:`Server.stream` handler
    (spec §9). ``progress`` is 0..1; ``stage`` names the pipeline phase; optional
    ``partial`` carries early hits. Serialised into a ``notifications/progress``
    frame carrying the request's ``progressToken``."""

    __slots__ = ("progress", "stage", "partial")

    def __init__(self, progress: float, stage: str = "", partial=None):
        self.progress = progress
        self.stage = stage
        self.partial = partial


def make_progress_notification(token, progress: float, stage: str = "", partial=None) -> str:
    """Build a ``notifications/progress`` frame (spec §9) during a long retrieve.
    ``token`` echoes the request's ``_meta.progressToken``; ``progress`` is 0..1."""
    params = {"progressToken": token, "progress": progress}
    if stage:
        params["stage"] = stage
    if partial is not None:
        params["partial"] = partial
    return _dumps({"jsonrpc": "2.0", "method": Method.PROGRESS, "params": params})


# Count-like params that share one rule across every method (spec §4.6/§7.7): a
# value out of RANGE is clamped by the server, but a value of the wrong TYPE or
# sign is structurally invalid and MUST be -32602. Validating centrally means no
# handler can forget, and all four SDKs reject the same inputs identically.
_POSITIVE_INT_FIELDS = ("k", "topN", "candidateK", "n", "hops", "tokenBudget", "limit")


def _validate_params(method: str, params):
    """Return an ``(code, message, data)`` triple if *params* is structurally
    invalid for *method*, else ``None``. Enforces only method-independent
    invariants the spec states as MUST; semantic checks stay in the handler."""
    if not isinstance(params, dict):
        return (Errc.INVALID_PARAMS, "'params' MUST be an object", None)

    for field in _POSITIVE_INT_FIELDS:
        if field not in params:
            continue
        v = params[field]
        if isinstance(v, bool) or not isinstance(v, int):
            return (Errc.INVALID_PARAMS,
                    f"'{field}' MUST be an integer", {"field": field})
        if v < 1:
            return (Errc.INVALID_PARAMS,
                    f"'{field}' MUST be >= 1", {"field": field})

    # §3.3 funnel invariant: candidateK >= rerank.topN >= k.
    k = params.get("k")
    cand = params.get("candidateK")
    rerank = params.get("rerank")
    top_n = rerank.get("topN") if isinstance(rerank, dict) else None
    if isinstance(top_n, bool) or not isinstance(top_n, int):
        top_n = None
    if isinstance(k, int) and isinstance(cand, int) and cand < k:
        return (Errc.INVALID_PARAMS,
                "'candidateK' MUST be >= 'k' (funnel invariant)", {"field": "candidateK"})
    if isinstance(k, int) and top_n is not None and top_n < k:
        return (Errc.INVALID_PARAMS,
                "'rerank.topN' MUST be >= 'k' (funnel invariant)", {"field": "rerank.topN"})
    if isinstance(cand, int) and top_n is not None and cand < top_n:
        return (Errc.INVALID_PARAMS,
                "'candidateK' MUST be >= 'rerank.topN' (funnel invariant)",
                {"field": "candidateK"})
    return None


class Server:
    """An RCP/1 server. Register handlers with :meth:`on` (usable as a decorator),
    then serve over stdio or HTTP — or drive :meth:`handle` directly for tests."""

    def __init__(self):
        self._info = {"name": "unknown", "version": "0"}
        self._caps: dict = {}          # wire JSON key -> metadata object
        self._handlers: dict = {}      # method string -> callable(params) -> result
        self._streamers: dict = {}     # method string -> generator(params) -> progress.., result
        self._initialized = False
        self._negotiated = PROTOCOL_VERSION   # version agreed at handshake (§7.1)

    # ── configuration ───────────────────────────────────────────────────────
    def set_info(self, name: str, version: str) -> None:
        self._info = {"name": str(name), "version": str(version)}

    def set_conformance(self, level: str) -> None:
        """Declare the conformance level (spec §14) this server claims: ``"L0"``,
        ``"L1"``, or ``"L2"``. Surfaced in ``initialize``/``info`` under
        ``_meta.conformance`` — the conformance harness holds the server to it."""
        if level not in ("L0", "L1", "L2"):
            raise ValueError("conformance level must be one of L0, L1, L2")
        self._conformance = level

    def advertise(self, capability, meta=None) -> None:
        """Advertise a capability with optional metadata (stored under its wire
        key). Advertising is independent of registering a handler; a gated call
        needs both or it is CapabilityMissing."""
        self._caps[cap_key(capability)] = {} if meta is None else meta

    def on(self, method, fn=None):
        """Register a handler for ``method``. Works as a decorator
        (``@s.on("retrieve")``) or directly (``s.on("retrieve", fn)``). The
        handler takes the params dict and returns the result object."""
        name = method.value if isinstance(method, Capability) else str(method)
        if name not in _KNOWN_METHODS:
            raise ValueError(f"unknown method hook: {name!r} "
                             f"(expected one of {sorted(_KNOWN_METHODS)})")
        if fn is None:
            def deco(f):
                self._handlers[name] = f
                return f
            return deco
        self._handlers[name] = fn
        return fn

    def stream(self, method, fn=None):
        r"""Register a STREAMING handler for ``method`` (spec §13 SSE transport).

        The handler is a generator: it ``yield``s zero or more :class:`Progress`
        objects (emitted as ``notifications/progress`` frames) and then
        ``return``\ s the final result object. Over an SSE request each progress
        frame is flushed as it is produced, followed by one final response
        frame; over a plain unary request the progress frames are drained and
        only the final result is returned, so the same handler works both ways.
        Requires the ``streaming`` capability to be advertised.
        """
        name = method.value if isinstance(method, Capability) else str(method)
        if name not in _KNOWN_METHODS:
            raise ValueError(f"unknown method hook: {name!r}")
        if fn is None:
            def deco(f):
                self._streamers[name] = f
                return f
            return deco
        self._streamers[name] = fn
        return fn

    # ── dispatch ─────────────────────────────────────────────────────────────────
    def handle(self, request):
        """Handle one JSON-RPC request object → reply dict, or ``None`` for a
        notification that warrants no response. Never raises."""
        id_ = request.get("id") if isinstance(request, dict) else None
        if not isinstance(request, dict) or not isinstance(request.get("method"), str):
            return _err(id_, Errc.INVALID_REQUEST, "missing 'method'")
        m = request["method"]
        params = request["params"] if "params" in request else {}

        # §4.5: a message with NO `id` is a notification and MUST NOT be
        # answered — whatever it asks for, known method or not. This check must
        # precede all dispatch: answering a notification (even with an error)
        # desynchronises a pipelining client, which correlates strictly by id.
        if "id" not in request:
            return None

        if m == Method.INITIALIZE:
            neg = negotiate_version(params.get("protocolVersion", PROTOCOL_VERSION)
                                    if isinstance(params, dict) else PROTOCOL_VERSION)
            if neg < MIN_PROTOCOL_VERSION:
                return _err(id_, Errc.VERSION_MISMATCH, "no common version")
            # §7.1: the session is initialized only on a SUCCESSFUL handshake.
            # Setting this before the version check would leave a server that
            # just rejected the peer's version fully unlocked.
            self._initialized = True
            self._negotiated = neg
            return _ok(id_, self._info_result(neg))
        if m == Method.INFO:
            return _ok(id_, self._info_result(PROTOCOL_VERSION))
        if m == Method.PING:
            has_nonce = isinstance(params, dict) and "nonce" in params
            return _ok(id_, {"nonce": params["nonce"]} if has_nonce else {})
        if m == Method.CANCEL:
            # `notifications/cancel` carrying an `id` is malformed: §7.14 defines
            # it as a notification only. Reject rather than silently accept.
            return _err(id_, Errc.INVALID_REQUEST,
                        "'notifications/cancel' is a notification and MUST NOT carry an 'id'")
        if m == Method.SHUTDOWN:
            # §7.12/§13: shutdown is always available. It is the graceful twin of
            # EOF-on-stdin (which is unconditional), leaks nothing, and refusing
            # it would force a peer to either handshake pointlessly or hard-kill.
            return _ok(id_, {})

        if not self._initialized:
            return _err(id_, Errc.NOT_INITIALIZED, "call 'initialize' first")

        return self._dispatch(id_, m, params if isinstance(params, dict) else {})

    def _dispatch(self, id_, m, params):
        cap = CAP_FOR_METHOD.get(m)
        if cap is None:
            return _err(id_, Errc.UNKNOWN_METHOD, f"unknown method '{m}'")
        fn = self._handlers.get(m)
        if fn is None and m in self._streamers:
            # A streaming-only handler still answers a plain unary request: drain
            # its progress frames and return just the final result (spec §13 —
            # a non-SSE client MUST get a single buffered JSON response).
            fn = self._make_unary_from_stream(self._streamers[m])
        if fn is None:
            return _err(id_, Errc.CAPABILITY_MISSING, f"'{m}' not implemented")
        if self._caps.get(cap.value) is None:
            return _err(id_, Errc.CAPABILITY_MISSING, f"capability '{cap.value}' not supported")
        # §12: params are validated only after capability gating, so a client
        # learns "I don't do that at all" before "your argument was malformed".
        bad = _validate_params(m, params)
        if bad is not None:
            return _err(id_, bad[0], bad[1], bad[2])
        try:
            result = fn(params)
        except RcpError as e:
            return _err(id_, e.code, e.message, e.data)
        except Exception as e:  # a handler bug is an Internal error, never a crash
            return _err(id_, Errc.INTERNAL_ERROR, str(e))
        return _ok(id_, result if result is not None else {})

    @staticmethod
    def _make_unary_from_stream(gen_fn):
        """Wrap a generator streaming handler as a plain unary handler: run it to
        completion, discard the progress frames, and return its final result
        (the generator's ``return`` value, surfaced via StopIteration.value)."""
        def unary(params):
            gen = gen_fn(params)
            try:
                while True:
                    next(gen)
            except StopIteration as stop:
                return stop.value if stop.value is not None else {}
        return unary

    def handle_line(self, line: str) -> str:
        """Parse a JSON line, dispatch (single or batch), and return the reply
        string ("" when there is no response, e.g. an all-notification batch)."""
        try:
            msg = json.loads(line)
        except ValueError as e:
            return _dumps(_err(None, Errc.PARSE_ERROR, str(e)))

        if isinstance(msg, list):
            if not msg:
                return _dumps(_err(None, Errc.INVALID_REQUEST, "empty batch"))
            out = []
            for el in msg:
                reply = self.handle(el)
                if reply is not None:
                    out.append(reply)
            return _dumps(out) if out else ""

        reply = self.handle(msg)
        return _dumps(reply) if reply is not None else ""

    def _info_result(self, version: int) -> dict:
        result = {
            "protocolVersion": version,
            "server": dict(self._info),
            "capabilities": dict(self._caps),
        }
        if getattr(self, "_conformance", None):
            result["_meta"] = {"conformance": self._conformance}
        return result

    # ── serving ──────────────────────────────────────────────────────────────
    def serve_stdio(self) -> None:
        """Serve newline-delimited JSON-RPC over stdin/stdout until shutdown/EOF."""
        import sys

        stdin = sys.stdin.buffer
        stdout = sys.stdout.buffer
        for raw in stdin:
            line = raw.decode("utf-8", "replace").strip()
            if not line:
                continue
            is_shutdown = False
            try:
                parsed = json.loads(line)
                is_shutdown = isinstance(parsed, dict) and parsed.get("method") == Method.SHUTDOWN
            except ValueError:
                pass
            reply = self.handle_line(line)
            if reply:
                try:
                    stdout.write((reply + "\n").encode("utf-8"))
                    stdout.flush()
                except (BrokenPipeError, OSError):
                    break  # peer gone
            if is_shutdown:
                break

    def serve_http(self, port: int) -> None:
        """Serve JSON-RPC over minimal HTTP/1.1 on ``127.0.0.1:port``. A response
        returns 200; a notification (no reply) returns 204. Loops until killed."""
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("127.0.0.1", port))
        srv.listen(16)
        try:
            while True:
                conn, _ = srv.accept()
                try:
                    self._serve_http_conn(conn)
                except OSError:
                    pass
                finally:
                    conn.close()
        finally:
            srv.close()

    def _serve_http_conn(self, conn: socket.socket) -> None:
        conn.settimeout(30.0)
        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = conn.recv(4096)
            if not chunk:
                return
            buf += chunk
        head, _, rest = buf.partition(b"\r\n\r\n")
        length = 0
        accept = b""
        for hl in head.split(b"\r\n")[1:]:
            name, _, value = hl.partition(b":")
            key = name.strip().lower()
            if key == b"content-length":
                try:
                    length = int(value.strip())
                except ValueError:
                    length = 0
            elif key == b"accept":
                accept = value.strip().lower()
        body = rest
        while len(body) < length:
            chunk = conn.recv(4096)
            if not chunk:
                break
            body += chunk
        text = body[:length].decode("utf-8", "replace") if length else (body.decode("utf-8", "replace") or "{}")
        if not text.strip():
            text = "{}"

        # §13 SSE: if the client asked for text/event-stream, we advertised
        # `streaming`, and the request targets a registered streaming handler,
        # stream progress frames then the final response frame. Else buffer.
        if b"text/event-stream" in accept and self._try_serve_sse(conn, text):
            return

        reply = self.handle_line(text)
        if reply:
            payload = reply.encode("utf-8")
            resp = (b"HTTP/1.1 200 OK\r\n"
                    b"Content-Type: application/json\r\n"
                    b"Content-Length: " + str(len(payload)).encode() + b"\r\n"
                    b"Connection: close\r\n\r\n" + payload)
        else:
            resp = (b"HTTP/1.1 204 No Content\r\n"
                    b"Connection: close\r\n\r\n")
        conn.sendall(resp)

    def _try_serve_sse(self, conn: socket.socket, text: str) -> bool:
        """Attempt to serve *text* as an SSE stream. Returns True if it was
        handled as a stream (headers + frames written), False if the request is
        not eligible (caller then falls back to a buffered JSON response)."""
        try:
            req = json.loads(text)
        except ValueError:
            return False
        if not isinstance(req, dict):
            return False
        method = req.get("method")
        if method not in self._streamers:
            return False
        if self._caps.get(Capability.Streaming.value) is None:
            return False
        if not self._initialized and CAP_FOR_METHOD.get(method) is not None:
            return False
        params = req.get("params") or {}
        token = None
        meta = params.get("_meta") if isinstance(params, dict) else None
        if isinstance(meta, dict):
            token = meta.get("progressToken")

        conn.sendall(b"HTTP/1.1 200 OK\r\n"
                     b"Content-Type: text/event-stream\r\n"
                     b"Cache-Control: no-cache\r\n"
                     b"Connection: close\r\n\r\n")

        def frame(obj) -> bytes:
            return b"data: " + _dumps(obj).encode("utf-8") + b"\n\n"

        gen = self._streamers[method](params)
        result = {}
        try:
            while True:
                try:
                    ev = next(gen)
                except StopIteration as stop:
                    result = stop.value if stop.value is not None else {}
                    break
                if isinstance(ev, Progress) and token is not None:
                    n = make_progress_notification(token, ev.progress, ev.stage, ev.partial)
                    conn.sendall(b"data: " + n.encode("utf-8") + b"\n\n")
            reply = _ok(req.get("id"), result)
        except RcpError as e:
            reply = _err(req.get("id"), e.code, str(e), getattr(e, "data", None))
        except Exception as e:  # noqa: BLE001 — total: never crash the connection
            reply = _err(req.get("id"), Errc.INTERNAL_ERROR, str(e))
        conn.sendall(frame(reply))
        return True
