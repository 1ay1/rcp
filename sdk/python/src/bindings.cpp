// src/bindings.cpp — pybind11 bindings over the type-theoretic C++ RCP SDK.
//
// Exposes:
//   rcp.Client            — connect_stdio / connect_http, then typed calls.
//   rcp.Capability        — the capability enum for supports() checks.
//   rcp.PyServer          — a server whose method hooks are Python callables,
//                           so a Python RAG engine is a first-class RCP server.
//
// JSON crosses the boundary as Python dict/list/str/num via a nlohmann<->py
// converter, so callers work in native Python objects, not JSON strings.

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/functional.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "rcp.hpp"

namespace py = pybind11;
using namespace rcp;

// ── JSON <-> Python object conversion ────────────────────────────────────────
static py::object json_to_py(const Json& j) {
    switch (j.type()) {
        case Json::value_t::null:            return py::none();
        case Json::value_t::boolean:         return py::bool_(j.get<bool>());
        case Json::value_t::number_integer:  return py::int_(j.get<std::int64_t>());
        case Json::value_t::number_unsigned: return py::int_(j.get<std::uint64_t>());
        case Json::value_t::number_float:    return py::float_(j.get<double>());
        case Json::value_t::string:          return py::str(j.get<std::string>());
        case Json::value_t::array: {
            py::list l;
            for (const auto& e : j) l.append(json_to_py(e));
            return l;
        }
        case Json::value_t::object: {
            py::dict d;
            for (auto it = j.begin(); it != j.end(); ++it) d[py::str(it.key())] = json_to_py(it.value());
            return d;
        }
        default: return py::none();
    }
}

static Json py_to_json(const py::handle& o) {
    if (o.is_none()) return Json(nullptr);
    if (py::isinstance<py::bool_>(o)) return Json(o.cast<bool>());
    if (py::isinstance<py::int_>(o)) return Json(o.cast<std::int64_t>());
    if (py::isinstance<py::float_>(o)) return Json(o.cast<double>());
    if (py::isinstance<py::str>(o)) return Json(o.cast<std::string>());
    if (py::isinstance<py::list>(o) || py::isinstance<py::tuple>(o)) {
        Json a = Json::array();
        for (const auto& e : o) a.push_back(py_to_json(e));
        return a;
    }
    if (py::isinstance<py::dict>(o)) {
        Json d = Json::object();
        for (auto item : o.cast<py::dict>()) d[item.first.cast<std::string>()] = py_to_json(item.second);
        return d;
    }
    return Json(nullptr);
}

// Raise a Python exception from an RCP Error.
[[noreturn]] static void raise(const Error& e) {
    throw std::runtime_error("[RCP " + std::to_string(e.code) + "] " + e.message);
}

// ── A Python-backed server handler ───────────────────────────────────────────
// Holds Python callables for each optional method; capabilities are derived
// from which callables were supplied, so you can't advertise what you didn't
// provide. Models the C++ Handler concept via the always-present info/caps.
struct PyHandler {
    PeerInfo peer;
    Capabilities caps;
    std::function<py::object(py::object)> f_embed, f_rerank, f_retrieve, f_transform, f_graph;

    PeerInfo info() const { return peer; }
    Capabilities capabilities() const { return caps; }

    Result<Json> invoke(const std::function<py::object(py::object)>& f, const Json& params) {
        py::gil_scoped_acquire gil;
        try {
            py::object out = f(json_to_py(params));
            return py_to_json(out);
        } catch (const std::exception& e) {
            return fail<Json>(errc::InternalError, e.what());
        }
    }
    Result<Json> embed(const Json& p)     { return invoke(f_embed, p); }
    Result<Json> rerank(const Json& p)    { return invoke(f_rerank, p); }
    Result<Json> retrieve(const Json& p)  { return invoke(f_retrieve, p); }
    Result<Json> transform(const Json& p) { return invoke(f_transform, p); }
    Result<Json> graph(const Json& p)     { return invoke(f_graph, p); }
};

// Wrapper exposed to Python.
struct PyServer {
    std::optional<Server<PyHandler>> srv;
    PyHandler handler;

    void serve_stdio() {
        srv.emplace(handler);
        py::gil_scoped_release rel;
        srv->serve_stdio();
    }
    void serve_http(std::uint16_t port) {
        srv.emplace(handler);
        py::gil_scoped_release rel;
        (void)srv->serve_http(port);
    }
    // Single-shot handler for embedding / testing.
    py::object handle(py::dict request) {
        if (!srv) srv.emplace(handler);
        return json_to_py(srv->handle(py_to_json(request)));
    }
};

// ── Client wrapper ───────────────────────────────────────────────────────────
struct PyClient {
    std::optional<Client> cli;

    void connect_stdio(std::vector<std::string> argv) {
        auto r = Client::connect_stdio(std::move(argv));
        if (!r) raise(r.error());
        cli.emplace(std::move(*r));
    }
    void connect_http(std::string url) {
        auto r = Client::connect_http(std::move(url));
        if (!r) raise(r.error());
        cli.emplace(std::move(*r));
    }
    int protocol_version() const { return cli ? cli->protocol_version() : 0; }
    py::dict server() const {
        py::dict d; d["name"] = cli->server().name; d["version"] = cli->server().version; return d;
    }
    py::object capabilities() const { return json_to_py(cli->capabilities().to_json()); }
    bool supports(Capability c) const { return cli && cli->supports(c); }

    py::object embed(std::vector<std::string> texts) {
        auto r = cli->embed(texts);
        if (!r) raise(r.error());
        py::list out;
        for (auto& v : *r) { py::list row; for (float x : v) row.append(x); out.append(row); }
        return out;
    }
    py::object rerank(std::string query, std::vector<std::string> passages) {
        auto r = cli->rerank(query, passages);
        if (!r) raise(r.error());
        py::list out; for (float x : *r) out.append(x); return out;
    }
    py::object retrieve(std::string query, std::size_t k, py::object opts) {
        auto tk = TopK::make(k);
        if (!tk) raise(tk.error());
        Json o = opts.is_none() ? Json::object() : py_to_json(opts);
        auto r = cli->retrieve(query, *tk, o);
        if (!r) raise(r.error());
        py::list out;
        for (auto& h : *r) {
            py::dict d;
            d["id"] = h.id; d["score"] = h.score.get(); d["text"] = h.text;
            if (!h.citation.is_null()) d["citation"] = json_to_py(h.citation);
            out.append(d);
        }
        return out;
    }
    py::object graph(std::string op, py::object params) {
        Json p = params.is_none() ? Json::object() : py_to_json(params);
        auto r = cli->graph(op, p);
        if (!r) raise(r.error());
        return json_to_py(*r);
    }
    py::object ping(py::object nonce) {
        Json n = nonce.is_none() ? Json(nullptr) : py_to_json(nonce);
        auto r = cli->ping(n);
        if (!r) raise(r.error());
        return json_to_py(*r);
    }
    void shutdown_() { if (cli) (void)cli->shutdown(); }

    static PyClient from_client(Client&& c) { PyClient p; p.cli.emplace(std::move(c)); return p; }
};

// ── Selector: choose ONE backend from several reachable engines ──────────────
struct PySelector {
    Selector sel;

    void add_stdio(std::string id, std::vector<std::string> argv, int priority) {
        sel.add(EngineSpec::stdio(std::move(id), std::move(argv), priority));
    }
    void add_http(std::string id, std::string url, int priority) {
        sel.add(EngineSpec::http(std::move(id), std::move(url), priority));
    }
    std::size_t size() const { return sel.size(); }

    PyClient select(const std::string& id) {
        auto c = sel.select(id);
        if (!c) raise(c.error());
        return PyClient::from_client(std::move(*c));
    }
    PyClient select_primary() {
        auto c = sel.select_primary();
        if (!c) raise(c.error());
        return PyClient::from_client(std::move(*c));
    }
    PyClient select_capable(Capability cap) {
        auto c = sel.select_capable(cap);
        if (!c) raise(c.error());
        return PyClient::from_client(std::move(*c));
    }

    void load_registry_string(const std::string& text) {
        auto s = Selector::from_registry_string(text);
        if (!s) raise(s.error());
        sel = std::move(*s);
    }
    void load_registry(const std::string& path) {
        auto s = Selector::from_registry_file(path);
        if (!s) raise(s.error());
        sel = std::move(*s);
    }
};

PYBIND11_MODULE(_rcp, m) {
    m.doc() = "Type-theoretic RCP (Retrieval Context Protocol) — Python bindings over the C++ SDK";
    m.attr("PROTOCOL_VERSION") = kProtocolVersion;

    py::enum_<Capability>(m, "Capability")
        .value("Embed", Capability::Embed)
        .value("SparseEmbed", Capability::SparseEmbed)
        .value("MultiVector", Capability::MultiVector)
        .value("Rerank", Capability::Rerank)
        .value("Retrieve", Capability::Retrieve)
        .value("Transform", Capability::Transform)
        .value("Graph", Capability::Graph)
        .value("Index", Capability::Index);

    py::class_<PyClient>(m, "Client")
        .def(py::init<>())
        .def("connect_stdio", &PyClient::connect_stdio, py::arg("argv"))
        .def("connect_http", &PyClient::connect_http, py::arg("base_url"))
        .def_property_readonly("protocol_version", &PyClient::protocol_version)
        .def_property_readonly("server", &PyClient::server)
        .def_property_readonly("capabilities", &PyClient::capabilities)
        .def("supports", &PyClient::supports, py::arg("capability"))
        .def("embed", &PyClient::embed, py::arg("texts"))
        .def("rerank", &PyClient::rerank, py::arg("query"), py::arg("passages"))
        .def("retrieve", &PyClient::retrieve, py::arg("query"), py::arg("k") = 10, py::arg("opts") = py::none())
        .def("graph", &PyClient::graph, py::arg("op"), py::arg("params") = py::none())
        .def("ping", &PyClient::ping, py::arg("nonce") = py::none())
        .def("shutdown", &PyClient::shutdown_);

    py::class_<PyServer>(m, "Server")
        .def(py::init<>())
        .def("set_info", [](PyServer& s, std::string name, std::string version) {
            s.handler.peer = PeerInfo{std::move(name), std::move(version)};
        }, py::arg("name"), py::arg("version"))
        .def("advertise", [](PyServer& s, Capability c, py::object meta) {
            Json m = meta.is_none() ? Json::object() : py_to_json(meta);
            auto& caps = s.handler.caps;
            switch (c) {
                case Capability::Embed:       caps.embed = m; break;
                case Capability::SparseEmbed: caps.sparse_embed = m; break;
                case Capability::MultiVector: caps.multi_vector = m; break;
                case Capability::Rerank:      caps.rerank = m; break;
                case Capability::Retrieve:    caps.retrieve = m; break;
                case Capability::Transform:   caps.transform = m; break;
                case Capability::Graph:       caps.graph = m; break;
                case Capability::Index:       caps.index = m; break;
            }
        }, py::arg("capability"), py::arg("meta") = py::none())
        .def("on", [](PyServer& s, const std::string& method, std::function<py::object(py::object)> fn) {
            if (method == "embed")     s.handler.f_embed = std::move(fn);
            else if (method == "rerank")    s.handler.f_rerank = std::move(fn);
            else if (method == "retrieve")  s.handler.f_retrieve = std::move(fn);
            else if (method == "transform") s.handler.f_transform = std::move(fn);
            else if (method == "graph")     s.handler.f_graph = std::move(fn);
            else throw std::runtime_error("unknown method hook: " + method);
        }, py::arg("method"), py::arg("fn"))
        .def("handle", &PyServer::handle, py::arg("request"))
        .def("serve_stdio", &PyServer::serve_stdio)
        .def("serve_http", &PyServer::serve_http, py::arg("port"));

    py::class_<PySelector>(m, "Selector")
        .def(py::init<>())
        .def_static("load", [](const std::string& path) {
            PySelector s; s.load_registry(path); return s;
        }, py::arg("path"), "Build a Selector from an rcp.json registry file (§16.1).")
        .def_static("loads", [](const std::string& text) {
            PySelector s; s.load_registry_string(text); return s;
        }, py::arg("text"), "Build a Selector from a registry JSON string.")
        .def("add_stdio", &PySelector::add_stdio, py::arg("id"), py::arg("argv"), py::arg("priority") = 0)
        .def("add_http", &PySelector::add_http, py::arg("id"), py::arg("url"), py::arg("priority") = 0)
        .def_property_readonly("size", &PySelector::size)
        .def("select", &PySelector::select, py::arg("id"))
        .def("select_primary", &PySelector::select_primary)
        .def("select_capable", &PySelector::select_capable, py::arg("capability"));
}
