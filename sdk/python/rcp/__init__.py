"""rcp — the Retrieval Context Protocol, Python SDK.

A thin, Pythonic layer over the type-theoretic C++ SDK (compiled native module
`_rcp`). RCP is an open, versioned JSON-RPC protocol so any RAG engine — any
language, any vendor — can expose embed/rerank/retrieve/graph/index, and any
client can consume it uniformly.

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
from ._rcp import Client, Server as _Server, Capability, PROTOCOL_VERSION, Selector

__all__ = ["Client", "Server", "Selector", "Capability", "PROTOCOL_VERSION",
           "Method", "Errc", "connect_stdio", "connect_http"]
__version__ = "1.0.0"


class Method:
    """RCP/1 method names (wire vocabulary, §7 of the spec)."""
    INITIALIZE   = "initialize"
    INFO         = "info"
    EMBED        = "embed"
    EMBED_SPARSE = "embed/sparse"
    EMBED_MULTI  = "embed/multi"
    RERANK       = "rerank"
    RETRIEVE     = "retrieve"
    TRANSFORM    = "query/transform"
    GRAPH        = "graph"
    INDEX_ADD    = "index/add"
    INDEX_DELETE = "index/delete"
    CATALOG_LIST = "catalog/list"
    SHUTDOWN     = "shutdown"


class Errc:
    """RCP/1 error codes (§12). -326xx are JSON-RPC; -320xx are RCP-specific."""
    PARSE_ERROR         = -32700
    INVALID_REQUEST     = -32600
    METHOD_NOT_FOUND    = -32601
    INVALID_PARAMS      = -32602
    INTERNAL_ERROR      = -32603
    NOT_INITIALIZED     = -32001
    VERSION_MISMATCH    = -32002
    CAPABILITY_MISSING  = -32003
    UNKNOWN_METHOD      = -32004
    OPTION_UNSUPPORTED  = -32005
    BACKEND_UNAVAILABLE = -32010
    RATE_LIMITED        = -32011


class Server(_Server):
    """RCP server. `on(method)` doubles as a decorator:

        @s.on("retrieve")
        def _(params): ...
    """

    def on(self, method, fn=None):
        if fn is None:
            def deco(f):
                super(Server, self).on(method, f)
                return f
            return deco
        return super().on(method, fn)


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
