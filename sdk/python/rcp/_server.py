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


def make_progress_notification(token, progress: float, stage: str = "", partial=None) -> str:
    """Build a ``notifications/progress`` frame (spec §9) during a long retrieve.
    ``token`` echoes the request's ``_meta.progressToken``; ``progress`` is 0..1."""
    params = {"progressToken": token, "progress": progress}
    if stage:
        params["stage"] = stage
    if partial is not None:
        params["partial"] = partial
    return _dumps({"jsonrpc": "2.0", "method": Method.PROGRESS, "params": params})


class Server:
    """An RCP/1 server. Register handlers with :meth:`on` (usable as a decorator),
    then serve over stdio or HTTP — or drive :meth:`handle` directly for tests."""

    def __init__(self):
        self._info = {"name": "unknown", "version": "0"}
        self._caps: dict = {}          # wire JSON key -> metadata object
        self._handlers: dict = {}      # method string -> callable(params) -> result
        self._initialized = False

    # ── configuration ───────────────────────────────────────────────────────
    def set_info(self, name: str, version: str) -> None:
        self._info = {"name": str(name), "version": str(version)}

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

    # ── dispatch ─────────────────────────────────────────────────────────────
    def handle(self, request):
        """Handle one JSON-RPC request object → reply dict, or ``None`` for a
        notification that warrants no response. Never raises."""
        id_ = request.get("id") if isinstance(request, dict) else None
        if not isinstance(request, dict) or not isinstance(request.get("method"), str):
            return _err(id_, Errc.INVALID_REQUEST, "missing 'method'")
        m = request["method"]
        params = request["params"] if "params" in request else {}

        if m == Method.INITIALIZE:
            self._initialized = True
            neg = negotiate_version(params.get("protocolVersion", PROTOCOL_VERSION)
                                    if isinstance(params, dict) else PROTOCOL_VERSION)
            if neg < MIN_PROTOCOL_VERSION:
                return _err(id_, Errc.VERSION_MISMATCH, "no common version")
            return _ok(id_, self._info_result(neg))
        if m == Method.INFO:
            return _ok(id_, self._info_result(PROTOCOL_VERSION))
        if m == Method.PING:
            has_nonce = isinstance(params, dict) and "nonce" in params
            return _ok(id_, {"nonce": params["nonce"]} if has_nonce else {})
        if m == Method.CANCEL:
            return None  # cancellation is a notification: never answered
        if m == Method.SHUTDOWN:
            return _ok(id_, {})

        if not self._initialized:
            return _err(id_, Errc.NOT_INITIALIZED, "call 'initialize' first")

        return self._dispatch(id_, m, params if isinstance(params, dict) else {})

    def _dispatch(self, id_, m, params):
        cap = CAP_FOR_METHOD.get(m)
        if cap is None:
            return _err(id_, Errc.UNKNOWN_METHOD, f"unknown method '{m}'")
        fn = self._handlers.get(m)
        if fn is None:
            return _err(id_, Errc.CAPABILITY_MISSING, f"'{m}' not implemented")
        if self._caps.get(cap.value) is None:
            return _err(id_, Errc.CAPABILITY_MISSING, f"capability '{cap.value}' not supported")
        try:
            result = fn(params)
        except RcpError as e:
            return _err(id_, e.code, e.message, e.data)
        except Exception as e:  # a handler bug is an Internal error, never a crash
            return _err(id_, Errc.INTERNAL_ERROR, str(e))
        return _ok(id_, result if result is not None else {})

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
        return {
            "protocolVersion": version,
            "server": dict(self._info),
            "capabilities": dict(self._caps),
        }

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
        for hl in head.split(b"\r\n")[1:]:
            name, _, value = hl.partition(b":")
            if name.strip().lower() == b"content-length":
                try:
                    length = int(value.strip())
                except ValueError:
                    length = 0
        body = rest
        while len(body) < length:
            chunk = conn.recv(4096)
            if not chunk:
                break
            body += chunk
        text = body[:length].decode("utf-8", "replace") if length else (body.decode("utf-8", "replace") or "{}")
        if not text.strip():
            text = "{}"
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
