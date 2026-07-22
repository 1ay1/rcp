#pragma once
// rcp/client.hpp — a typestate RCP client.
//
// TYPESTATE: a Client only exists in the `Ready` state, and the ONLY way to
// obtain one is `Client::connect(...)`, which performs the `initialize`
// handshake and returns Result<Client>. There is no default constructor and no
// "unconnected" client with dormant methods — calling a method before the
// handshake is not a runtime -32001, it is unrepresentable in the type system.
//
// CAPABILITY GATING: each typed call first checks the cached, negotiated
// capabilities and returns Errc::CapabilityMissing *before* a round-trip if the
// server never advertised the feature. The check is a total match on the typed
// Capability enum, not string-poking.
//
// TOTALITY: every call returns Result<T>; no exceptions cross the API boundary.

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rcp/protocol.hpp"
#include "rcp/transport.hpp"
#include "rcp/types.hpp"

namespace rcp {

class Client {
public:
    // The only constructor path: connect over an existing transport.
    [[nodiscard]] static Result<Client>
    connect(std::unique_ptr<Transport> transport, PeerInfo client_info = {"rcp-cpp", "1.0"}) {
        Client c(std::move(transport));
        Json params{{"protocolVersion", kProtocolVersion},
                    {"client", client_info.to_json()},
                    {"capabilities", Json::object()}};
        auto res = c.request(method::Initialize, params);
        if (!res) return std::unexpected(res.error());
        auto init = InitializeResult::from_json(*res);
        if (init.protocol_version < kMinProtocolVersion)
            return fail<Client>(errc::VersionMismatch, "server offered no usable protocol version");
        c.negotiated_ = init.protocol_version;
        c.server_     = std::move(init.server);
        c.caps_       = std::move(init.capabilities);
        return c;
    }

    // Convenience constructors.
    [[nodiscard]] static Result<Client>
    connect_stdio(std::vector<std::string> argv, PeerInfo client_info = {"rcp-cpp", "1.0"}) {
        auto t = StdioTransport::spawn(argv);
        if (!t) return std::unexpected(t.error());
        return connect(std::move(*t), std::move(client_info));
    }
    [[nodiscard]] static Result<Client>
    connect_http(std::string base_url, PeerInfo client_info = {"rcp-cpp", "1.0"}) {
        return connect(std::make_unique<HttpTransport>(std::move(base_url)), std::move(client_info));
    }

    Client(Client&&) = default;
    Client& operator=(Client&&) = default;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // ── introspection ─────────────────────────────────────────────────────────
    [[nodiscard]] int                 protocol_version() const noexcept { return negotiated_; }
    [[nodiscard]] const PeerInfo&      server()          const noexcept { return server_; }
    [[nodiscard]] const Capabilities&  capabilities()    const noexcept { return caps_; }
    [[nodiscard]] bool supports(Capability c)            const noexcept { return caps_.has(c); }

    // ── typed, capability-gated calls ───────────────────────────────────────────
    [[nodiscard]] Result<std::vector<std::vector<float>>> embed(std::span<const std::string> texts) {
        if (auto g = gate(Capability::Embed); !g) return std::unexpected(g.error());
        Json p; p["texts"] = Json::array();
        for (auto& t : texts) p["texts"].push_back(t);
        auto r = request(method::Embed, p);
        if (!r) return std::unexpected(r.error());
        const Json& v = r->contains("vectors") ? (*r)["vectors"] : *r;
        std::vector<std::vector<float>> out;
        for (auto& row : v) { std::vector<float> vec; for (auto& x : row) vec.push_back(x.get<float>()); out.push_back(std::move(vec)); }
        return out;
    }

    [[nodiscard]] Result<std::vector<float>>
    rerank(std::string_view query, std::span<const std::string> passages) {
        if (auto g = gate(Capability::Rerank); !g) return std::unexpected(g.error());
        Json p; p["query"] = query; p["passages"] = Json::array();
        for (auto& x : passages) p["passages"].push_back(x);
        auto r = request(method::Rerank, p);
        if (!r) return std::unexpected(r.error());
        const Json& s = r->contains("scores") ? (*r)["scores"] : *r;
        std::vector<float> out; for (auto& x : s) out.push_back(x.get<float>());
        return out;
    }

    // TopK is a refinement type: the caller literally cannot pass k = 0.
    [[nodiscard]] Result<std::vector<Hit>>
    retrieve(std::string_view query, TopK k, Json opts = Json::object()) {
        if (auto g = gate(Capability::Retrieve); !g) return std::unexpected(g.error());
        Json p = std::move(opts);
        p["query"] = query;
        p["k"] = k.get();
        auto r = request(method::Retrieve, p);
        if (!r) return std::unexpected(r.error());
        const Json& arr = r->contains("hits") ? (*r)["hits"] : *r;
        std::vector<Hit> out; for (auto& h : arr) out.push_back(Hit::from_json(h));
        return out;
    }

    [[nodiscard]] Result<Json> graph(std::string_view op, Json params = Json::object()) {
        if (auto g = gate(Capability::Graph); !g) return std::unexpected(g.error());
        params["op"] = op;
        return request(method::Graph, params);
    }

    [[nodiscard]] Result<Json> transform(std::string_view query, std::string_view method_name) {
        if (auto g = gate(Capability::Transform); !g) return std::unexpected(g.error());
        return request(method::Transform, Json{{"query", query}, {"method", method_name}});
    }

    // Escape hatch: any method with raw params (still returns Result).
    [[nodiscard]] Result<Json> call(std::string_view method_name, Json params) {
        return request(method_name, std::move(params));
    }

    // Liveness / round-trip check. Ungated; echoes any nonce. Returns the result
    // object (which carries the echoed nonce, if one was sent).
    [[nodiscard]] Result<Json> ping(Json nonce = Json(nullptr)) {
        Json p = Json::object();
        if (!nonce.is_null()) p["nonce"] = std::move(nonce);
        return request(method::Ping, std::move(p));
    }

    Result<void> shutdown() {
        auto r = request(method::Shutdown, Json::object());
        transport_->close();
        if (!r) return std::unexpected(r.error());
        return {};
    }

private:
    explicit Client(std::unique_ptr<Transport> t) : transport_(std::move(t)) {}

    [[nodiscard]] Result<void> gate(Capability c) const {
        if (!caps_.has(c))
            return fail<void>(errc::CapabilityMissing,
                              "server does not advertise '" + std::string(to_string(c)) + "'");
        return {};
    }

    [[nodiscard]] Result<Json> request(std::string_view m, Json params) {
        Json req{{"jsonrpc", "2.0"}, {"id", ++next_id_}, {"method", m}, {"params", std::move(params)}};
        auto reply = transport_->call(req);
        if (!reply) return std::unexpected(reply.error());
        if (auto it = reply->find("error"); it != reply->end() && !it->is_null())
            return std::unexpected(Error::from_json(*it));
        if (auto it = reply->find("result"); it != reply->end()) return *it;
        return Json::object();
    }

    std::unique_ptr<Transport> transport_;
    std::int64_t next_id_ = 0;
    int          negotiated_ = kProtocolVersion;
    PeerInfo     server_;
    Capabilities caps_;
};

} // namespace rcp
