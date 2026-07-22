"""RCP transports — the request/reply seam (subprocess pipe or HTTP/1.1).

A transport is a total function ``dict -> dict``: send one JSON-RPC request
object, return the reply object. Two concrete transports mirror the C++ SDK
(``sdk/cpp/include/rcp/transport.hpp``):

* :class:`StdioTransport` — newline-delimited JSON over a child's stdin/stdout.
* :class:`HttpTransport`   — minimal HTTP/1.1 POST to ``<base>/<method>``.

Failures raise :class:`RcpError` with the same typed codes as the C++ SDK, so a
caller sees ``[RCP -32010] ...`` whether the fault was local or remote.
"""
from __future__ import annotations

import json
import subprocess
import urllib.parse
from http.client import HTTPConnection

from ._types import Errc, RcpError

_COMPACT = (",", ":")  # compact JSON separators, matching nlohmann's dump()


def _dumps(obj) -> str:
    return json.dumps(obj, separators=_COMPACT)


class StdioTransport:
    """Newline-delimited JSON over a subprocess' stdio.

    The client writes ``json + "\\n"`` and reads reply lines, **skipping** any
    server-initiated notification (a frame carrying ``method``) and any stray
    reply whose ``id`` does not match the request (spec §4.7).
    """

    def __init__(self, proc: subprocess.Popen):
        self._proc = proc

    @classmethod
    def spawn(cls, argv) -> "StdioTransport":
        argv = list(argv)
        try:
            proc = subprocess.Popen(
                argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0,
            )
        except OSError as e:
            raise RcpError(Errc.BACKEND_UNAVAILABLE, f"cannot spawn {argv!r}: {e}")
        return cls(proc)

    def call(self, request: dict) -> dict:
        line = (_dumps(request) + "\n").encode("utf-8")
        try:
            self._proc.stdin.write(line)
            self._proc.stdin.flush()
        except (BrokenPipeError, OSError):
            raise RcpError(Errc.BACKEND_UNAVAILABLE, "write to server failed")

        want_id = request.get("id")
        while True:
            raw = self._proc.stdout.readline()
            if not raw:
                raise RcpError(Errc.BACKEND_UNAVAILABLE, "server closed the connection")
            try:
                reply = json.loads(raw)
            except ValueError as e:
                raise RcpError(Errc.PARSE_ERROR, str(e))
            if isinstance(reply, dict):
                # Skip a server-initiated notification (has "method", no reply id
                # to match) and a mismatched-id reply from an earlier request.
                if "method" in reply:
                    continue
                if want_id is not None and "id" in reply and reply["id"] != want_id:
                    continue
            return reply

    def close(self) -> None:
        proc = self._proc
        if proc.poll() is not None:
            return
        for stream in (proc.stdin, proc.stdout):
            try:
                if stream is not None:
                    stream.close()
            except OSError:
                pass
        try:
            proc.terminate()
            proc.wait(timeout=2)
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass


class HttpTransport:
    """Minimal HTTP/1.1 client: POST the request to ``<base_url>/<method>``.

    The server ignores the path and dispatches on the body's ``method``; the path
    is a human-readable convenience (e.g. ``POST /rcp/retrieve``). Maps HTTP 429
    → RateLimited and 503 → BackendUnavailable (spec §5.2).
    """

    def __init__(self, base_url: str, timeout: float = 30.0):
        self._base = base_url.rstrip("/")
        self._timeout = timeout

    def call(self, request: dict) -> dict:
        method = request.get("method", "")
        url = self._base + "/" + method
        parts = urllib.parse.urlsplit(url)
        if parts.scheme != "http":
            raise RcpError(Errc.INVALID_PARAMS, f"unsupported URL scheme '{parts.scheme}'")
        path = parts.path or "/"
        if parts.query:
            path += "?" + parts.query
        body = _dumps(request).encode("utf-8")

        conn = HTTPConnection(parts.hostname or "127.0.0.1", parts.port or 80,
                              timeout=self._timeout)
        try:
            conn.request("POST", path, body=body,
                         headers={"Content-Type": "application/json",
                                  "Content-Length": str(len(body))})
            resp = conn.getresponse()
            status = resp.status
            data = resp.read()
        except OSError as e:
            raise RcpError(Errc.BACKEND_UNAVAILABLE, f"HTTP request failed: {e}")
        finally:
            conn.close()

        if status == 429:
            raise RcpError(Errc.RATE_LIMITED, "HTTP 429")
        if status == 503:
            raise RcpError(Errc.BACKEND_UNAVAILABLE, "HTTP 503")
        if (status < 200 or status >= 300) and not data.strip():
            raise RcpError(Errc.BACKEND_UNAVAILABLE, f"HTTP {status}")
        try:
            return json.loads(data)
        except ValueError as e:
            raise RcpError(Errc.PARSE_ERROR, str(e))

    def close(self) -> None:  # HTTP is connectionless here; nothing to hold open.
        pass
