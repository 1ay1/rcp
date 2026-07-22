#pragma once
// rcp/server.hpp — a concept-constrained RCP server.
//
// A Handler is anything STRUCTURALLY satisfying the Handler concept: it exposes
// `info()`, `capabilities()`, and whichever method hooks it implements. The
// Server owns the JSON-RPC framing, the initialize handshake, capability gating,
// and error mapping; the handler provides retrieval logic and returns Result.
//
// Optional method hooks are detected at COMPILE time via concepts (has_embed<H>
// etc.): a hook you don't define is simply not called, and the corresponding
// capability check falls through to CapabilityMissing. There is no vtable of
// null function pointers to trip over.

#include <string>
#include <string_view>
#include <utility>

#include "rcp/protocol.hpp"
#include "rcp/types.hpp"

#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace rcp {

// ── Handler concept ──────────────────────────────────────────────────────────
// The required surface: identity + advertised capabilities.
template <class H>
concept HandlerBase = requires(const H& h) {
    { h.info() }         -> std::convertible_to<PeerInfo>;
    { h.capabilities() } -> std::convertible_to<Capabilities>;
};

// Optional method-hook detectors. A hook takes the params Json and returns a
// Result<Json>. Presence is a compile-time property.
template <class H> concept HasEmbed        = requires(H& h, const Json& p) { { h.embed(p) }        -> std::same_as<Result<Json>>; };
template <class H> concept HasEmbedSparse  = requires(H& h, const Json& p) { { h.embed_sparse(p) } -> std::same_as<Result<Json>>; };
template <class H> concept HasEmbedMulti   = requires(H& h, const Json& p) { { h.embed_multi(p) }  -> std::same_as<Result<Json>>; };
template <class H> concept HasRerank       = requires(H& h, const Json& p) { { h.rerank(p) }       -> std::same_as<Result<Json>>; };
template <class H> concept HasRetrieve     = requires(H& h, const Json& p) { { h.retrieve(p) }     -> std::same_as<Result<Json>>; };
template <class H> concept HasTransform    = requires(H& h, const Json& p) { { h.transform(p) }    -> std::same_as<Result<Json>>; };
template <class H> concept HasGraph        = requires(H& h, const Json& p) { { h.graph(p) }        -> std::same_as<Result<Json>>; };
template <class H> concept HasIndexAdd     = requires(H& h, const Json& p) { { h.index_add(p) }    -> std::same_as<Result<Json>>; };
template <class H> concept HasIndexDelete  = requires(H& h, const Json& p) { { h.index_delete(p) } -> std::same_as<Result<Json>>; };

template <class H>
concept Handler = HandlerBase<H>;

// ── Server ───────────────────────────────────────────────────────────────────
template <Handler H>
class Server {
public:
    explicit Server(H handler) : handler_(std::move(handler)) {}

    // Handle one JSON-RPC request object → reply object. Never throws.
    [[nodiscard]] Json handle(const Json& request) {
        Json id = request.contains("id") ? request["id"] : Json(nullptr);
        if (!request.is_object() || !request.contains("method") || !request["method"].is_string())
            return err(id, errc::InvalidRequest, "missing 'method'");
        const std::string m = request["method"].get<std::string>();
        const Json params = request.contains("params") ? request["params"] : Json::object();
        const Capabilities caps = handler_.capabilities();

        if (m == method::Initialize) {
            initialized_ = true;
            int neg = negotiate_version(params.value("protocolVersion", kProtocolVersion));
            if (neg < kMinProtocolVersion) return err(id, errc::VersionMismatch, "no common version");
            return ok(id, info_result(neg));
        }
        if (m == method::Info)     return ok(id, info_result(kProtocolVersion));
        if (m == method::Ping)     return ok(id, params.contains("nonce")
                                              ? Json{{"nonce", params["nonce"]}} : Json::object());
        if (m == method::Cancel) {
            // Cancellation is a notification: best-effort, never answered. The
            // dispatcher is synchronous here, so the target request has already
            // completed; treat as a no-op and emit no reply.
            return Json(nullptr);
        }
        if (m == method::Shutdown) return ok(id, Json::object());

        if (!initialized_) return err(id, errc::NotInitialized, "call 'initialize' first");

        // Capability-gated dispatch. Each arm is compiled only if the handler
        // models the hook; otherwise the capability is treated as absent.
        return dispatch(id, m, params, caps);
    }

    [[nodiscard]] std::string handle_line(const std::string& line) {
        try {
            Json reply = handle(Json::parse(line));
            if (reply.is_null()) return std::string{};   // notification: no response
            return reply.dump();
        }
        catch (const std::exception& e) { return err(Json(nullptr), errc::ParseError, e.what()).dump(); }
    }

    void serve_stdio() {
        std::string in;
        char tmp[4096];
        for (;;) {
            auto nl = in.find('\n');
            if (nl == std::string::npos) {
                ssize_t r = ::read(0, tmp, sizeof tmp);
                if (r <= 0) break;
                in.append(tmp, (size_t)r);
                continue;
            }
            std::string line = in.substr(0, nl); in.erase(0, nl + 1);
            if (line.empty()) continue;
            std::string reply = handle_line(line);
            if (!reply.empty()) {
                reply.push_back('\n');
                (void)!::write(1, reply.data(), reply.size());
            }
            try { if (Json::parse(line).value("method", std::string{}) == method::Shutdown) break; } catch (...) {}
        }
    }

    [[nodiscard]] Result<void> serve_http(std::uint16_t port) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return fail<void>(errc::BackendUnavailable, "socket() failed");
        int yes = 1; ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(port);
        if (::bind(fd, (sockaddr*)&a, sizeof a) != 0) { ::close(fd); return fail<void>(errc::BackendUnavailable, "bind failed"); }
        ::listen(fd, 16);
        for (;;) {
            int c = ::accept(fd, nullptr, nullptr);
            if (c < 0) continue;
            std::string req; char tmp[4096]; ssize_t r; std::size_t clen = 0; std::size_t hend = std::string::npos;
            while ((r = ::read(c, tmp, sizeof tmp)) > 0) {
                req.append(tmp, (size_t)r);
                if (hend == std::string::npos) {
                    hend = req.find("\r\n\r\n");
                    if (hend != std::string::npos) {
                        auto lc = req.find("Content-Length:");
                        if (lc == std::string::npos) lc = req.find("content-length:");
                        if (lc != std::string::npos) clen = std::strtoul(req.c_str() + lc + 15, nullptr, 10);
                    }
                }
                if (hend != std::string::npos && req.size() >= hend + 4 + clen) break;
            }
            std::string body = hend != std::string::npos ? req.substr(hend + 4) : std::string{};
            std::string reply = handle_line(body.empty() ? "{}" : body);
            if (reply.empty()) reply = "{}";   // notification: acknowledge with empty 200 body
            std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                               std::to_string(reply.size()) + "\r\nConnection: close\r\n\r\n" + reply;
            (void)!::write(c, resp.data(), resp.size());
            ::close(c);
        }
    }

private:
    Json info_result(int neg) {
        return InitializeResult{neg, handler_.info(), handler_.capabilities()}.to_json();
    }

    Json dispatch(const Json& id, const std::string& m, const Json& params, const Capabilities& caps) {
        auto run = [&](Capability cap, auto&& invoke) -> Json {
            if (!caps.has(cap))
                return err(id, errc::CapabilityMissing,
                           "capability '" + std::string(to_string(cap)) + "' not supported");
            Result<Json> r = invoke();
            if (!r) return Json{{"jsonrpc", "2.0"}, {"id", id}, {"error", r.error().to_json()}};
            return ok(id, *r);
        };

        if (m == method::Embed) {
            if constexpr (HasEmbed<H>)       return run(Capability::Embed,       [&]{ return handler_.embed(params); });
            else                             return err(id, errc::CapabilityMissing, "embed not implemented");
        }
        if (m == method::EmbedSparse) {
            if constexpr (HasEmbedSparse<H>) return run(Capability::SparseEmbed, [&]{ return handler_.embed_sparse(params); });
            else                             return err(id, errc::CapabilityMissing, "sparseEmbed not implemented");
        }
        if (m == method::EmbedMulti) {
            if constexpr (HasEmbedMulti<H>)  return run(Capability::MultiVector, [&]{ return handler_.embed_multi(params); });
            else                             return err(id, errc::CapabilityMissing, "multiVector not implemented");
        }
        if (m == method::Rerank) {
            if constexpr (HasRerank<H>)      return run(Capability::Rerank,      [&]{ return handler_.rerank(params); });
            else                             return err(id, errc::CapabilityMissing, "rerank not implemented");
        }
        if (m == method::Retrieve) {
            if constexpr (HasRetrieve<H>)    return run(Capability::Retrieve,    [&]{ return handler_.retrieve(params); });
            else                             return err(id, errc::CapabilityMissing, "retrieve not implemented");
        }
        if (m == method::Transform) {
            if constexpr (HasTransform<H>)   return run(Capability::Transform,   [&]{ return handler_.transform(params); });
            else                             return err(id, errc::CapabilityMissing, "transform not implemented");
        }
        if (m == method::Graph) {
            if constexpr (HasGraph<H>)       return run(Capability::Graph,       [&]{ return handler_.graph(params); });
            else                             return err(id, errc::CapabilityMissing, "graph not implemented");
        }
        if (m == method::IndexAdd) {
            if constexpr (HasIndexAdd<H>)    return run(Capability::Index,       [&]{ return handler_.index_add(params); });
            else                             return err(id, errc::CapabilityMissing, "index not implemented");
        }
        if (m == method::IndexDelete) {
            if constexpr (HasIndexDelete<H>) return run(Capability::Index,       [&]{ return handler_.index_delete(params); });
            else                             return err(id, errc::CapabilityMissing, "index not implemented");
        }
        return err(id, errc::UnknownMethod, "unknown method '" + m + "'");
    }

    static Json ok(const Json& id, Json result) {
        return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
    }
    static Json err(const Json& id, int code, std::string msg) {
        return Json{{"jsonrpc", "2.0"}, {"id", id}, {"error", Json{{"code", code}, {"message", std::move(msg)}}}};
    }

    H handler_;
    bool initialized_ = false;
};

// Deduction guide so `Server srv{MyHandler{}}` works.
template <class H> Server(H) -> Server<H>;

} // namespace rcp
