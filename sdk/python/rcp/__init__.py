"""rcp — the Retrieval Context Protocol, a native Python SDK.

Pure Python, zero dependencies (standard library only): no compiler, no C++, no
build step to ``pip install``. RCP is an open, versioned JSON-RPC protocol so any
RAG engine — any language, any vendor — can expose embed / rerank / retrieve /
graph / index / catalog, and any client can consume it uniformly. This SDK speaks
the exact same wire format as the type-theoretic C++ SDK, so a Python client and
a C++ server (or vice-versa) interoperate byte-for-byte.

Client (connects to any RCP server, over a subprocess or HTTP):

    import rcp
    c = rcp.connect_stdio(["python3", "my_server.py"])
    print(c.server, c.capabilities)
    if c.supports(rcp.Capability.Retrieve):
        for hit in c.retrieve("eiffel tower", k=3):
            print(hit["id"], hit["score"])

Server (expose a Python RAG engine as an RCP server):

    import rcp
    s = rcp.Server()
    s.set_info("my-engine", "1.0")
    s.advertise(rcp.Capability.Retrieve, {"maxK": 100, "modes": ["hybrid"]})

    @s.on("retrieve")
    def _(params):
        hits = my_index.search(params["query"], params.get("k", 10))
        return {"hits": [{"id": h.id, "score": h.score, "text": h.text} for h in hits]}

    s.serve_stdio()
"""
from ._client import Client
from . import _filter as filter  # noqa: A004 — public submodule `rcp.filter`
from ._selector import EngineSpec, Selector
from ._server import Server, make_log_notification, make_progress_notification
from ._transport import HttpTransport, StdioTransport
from ._types import (
    PROTOCOL_VERSION,
    Capability,
    Errc,
    Method,
    RcpError,
    negotiate_version,
)

__all__ = [
    "Client", "Server", "Selector", "EngineSpec", "Capability", "PROTOCOL_VERSION",
    "Method", "Errc", "RcpError", "connect_stdio", "connect_http",
    "make_log_notification", "make_progress_notification",
    "StdioTransport", "HttpTransport", "negotiate_version", "filter",
]
__version__ = "1.0.0"


def connect_stdio(argv, **kwargs) -> Client:
    """Spawn an RCP server subprocess and connect. `argv` e.g. ["python3","srv.py"]."""
    c = Client()
    c.connect_stdio(list(argv))
    return c


def connect_http(base_url: str, **kwargs) -> Client:
    """Connect to an HTTP RCP server, e.g. "http://127.0.0.1:8000/rcp"."""
    c = Client()
    c.connect_http(base_url)
    return c
