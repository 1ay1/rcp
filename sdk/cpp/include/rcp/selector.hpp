#pragma once
// rcp/selector.hpp — choose ONE RCP backend from several reachable engines.
//
// Where federation.hpp fans a query out to EVERY engine and fuses, a Selector
// solves the simpler and very common case: you have two (or more) RAG backends
// and the client should pick ONE to use as its backend — by name, by required
// capability, or by priority with liveness fallback (primary → secondary).
//
// Discovery is the caller's job (build specs from an rcp.json registry or by
// hand). Connection is LAZY: a backend is dialled only when it is actually
// selected, so listing five candidates does not open five subprocesses. The
// chosen result is a fully-connected, handshaken `Client` (rcp/client.hpp),
// which you then use exactly as a single-backend client.

#include <functional>
#include <algorithm>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "rcp/client.hpp"
#include "rcp/protocol.hpp"
#include "rcp/types.hpp"

namespace rcp {

// How to reach one backend. A spec is inert until selected; `open()` performs
// the connect + handshake on demand.
struct EngineSpec {
    std::string id;                        // client-local label ("docs", "web")
    // Transport: exactly one of argv (stdio subprocess) or url (http) is set.
    std::vector<std::string> argv;         // e.g. {"python3", "docs_server.py"}
    std::string url;                       // e.g. "http://127.0.0.1:8000/rcp"
    int priority = 0;                      // higher = preferred (for by_priority)
    double weight = 1.0;                   // fusion weight when federated (§16.3)
    bool required = false;                 // if true, a failure fails the federation

    [[nodiscard]] Result<Client> open(PeerInfo client_info = {"rcp-cpp", "1.0"}) const {
        if (!argv.empty()) return Client::connect_stdio(argv, std::move(client_info));
        if (!url.empty())  return Client::connect_http(url, std::move(client_info));
        return fail<Client>(errc::InvalidParams, "engine '" + id + "' has no transport");
    }

    static EngineSpec stdio(std::string id, std::vector<std::string> argv, int priority = 0) {
        return EngineSpec{std::move(id), std::move(argv), {}, priority, 1.0, false};
    }
    static EngineSpec http(std::string id, std::string url, int priority = 0) {
        return EngineSpec{std::move(id), {}, std::move(url), priority, 1.0, false};
    }

    // Parse one registry entry (§16.1). `transport` is "stdio" (needs `command`)
    // or "http" (needs `url`). `weight`/`required` are optional.
    [[nodiscard]] static Result<EngineSpec> from_json(const Json& j) {
        if (!j.is_object() || !j.contains("id"))
            return fail<EngineSpec>(errc::InvalidParams, "registry entry needs an 'id'");
        EngineSpec s;
        s.id       = j.value("id", std::string{});
        s.priority = j.value("priority", 0);
        s.weight   = j.value("weight", 1.0);
        s.required = j.value("required", false);
        const std::string transport = j.value("transport", std::string{});
        if (transport == "stdio" || (transport.empty() && j.contains("command"))) {
            if (!j.contains("command") || !j.at("command").is_array())
                return fail<EngineSpec>(errc::InvalidParams, "stdio engine '" + s.id + "' needs 'command' array");
            for (const auto& a : j.at("command")) s.argv.push_back(a.get<std::string>());
        } else if (transport == "http" || (transport.empty() && j.contains("url"))) {
            s.url = j.value("url", std::string{});
            if (s.url.empty())
                return fail<EngineSpec>(errc::InvalidParams, "http engine '" + s.id + "' needs 'url'");
        } else {
            return fail<EngineSpec>(errc::InvalidParams, "engine '" + s.id + "' has unknown transport '" + transport + "'");
        }
        return s;
    }
};

// A Selector holds candidate specs and returns exactly one connected Client per
// a policy. Nothing is connected until you call a select_* method.
class Selector {
public:
    Selector& add(EngineSpec spec) { specs_.push_back(std::move(spec)); return *this; }

    [[nodiscard]] std::size_t size() const noexcept { return specs_.size(); }
    [[nodiscard]] const std::vector<EngineSpec>& engines() const noexcept { return specs_; }

    // ── build from an rcp.json registry document (§16.1) ─────────────────────────
    // Parse a `{ "engines": [ ... ] }` document into a Selector. Every entry must
    // parse; a malformed entry fails the whole load (fail fast on config errors).
    [[nodiscard]] static Result<Selector> from_registry(const Json& doc) {
        if (!doc.is_object() || !doc.contains("engines") || !doc.at("engines").is_array())
            return fail<Selector>(errc::InvalidParams, "registry needs an 'engines' array");
        Selector sel;
        for (const auto& e : doc.at("engines")) {
            auto spec = EngineSpec::from_json(e);
            if (!spec) return std::unexpected(spec.error());
            sel.add(std::move(*spec));
        }
        return sel;
    }

    // Parse a registry from a JSON string.
    [[nodiscard]] static Result<Selector> from_registry_string(std::string_view text) {
        Json doc = Json::parse(text, nullptr, /*allow_exceptions=*/false);
        if (doc.is_discarded())
            return fail<Selector>(errc::ParseError, "registry is not valid JSON");
        return from_registry(doc);
    }

    // Load a registry from a file path (e.g. "rcp.json").
    [[nodiscard]] static Result<Selector> from_registry_file(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return fail<Selector>(errc::BackendUnavailable, "cannot open registry '" + path + "'");
        std::ostringstream ss;
        ss << in.rdbuf();
        return from_registry_string(ss.str());
    }

    // ── choose by explicit id ───────────────────────────────────────────────────
    [[nodiscard]] Result<Client> select(std::string_view id) const {
        for (const auto& s : specs_)
            if (s.id == id) return s.open();
        return fail<Client>(errc::MethodNotFound, "no engine named '" + std::string(id) + "'");
    }

    // ── choose the first that connects AND advertises `cap` ──────────────────────
    // Tries candidates in descending priority; skips one that can't connect or
    // lacks the capability. This is the "any backend that can do X" selector.
    [[nodiscard]] Result<Client> select_capable(Capability cap) const {
        Error last{errc::BackendUnavailable, "no candidate engine matched"};
        for (const auto* s : by_priority()) {
            auto c = s->open();
            if (!c) { last = c.error(); continue; }
            if (c->supports(cap)) return c;               // matched
            // connected but lacks capability: close (dtor) and try next
            last = Error{errc::CapabilityMissing, "engine '" + s->id + "' lacks required capability"};
        }
        return std::unexpected(last);
    }

    // ── primary → secondary → … with liveness fallback ─────────────────────────
    // Return the highest-priority backend that successfully connects. This is the
    // classic "use the fast/primary engine, fall back to the backup if it's down".
    [[nodiscard]] Result<Client> select_primary() const {
        Error last{errc::BackendUnavailable, "no engine available"};
        for (const auto* s : by_priority()) {
            auto c = s->open();
            if (c) return c;
            last = c.error();
        }
        return std::unexpected(last);
    }

    // ── choose by an arbitrary predicate over the connected client ──────────────
    // The predicate sees a live client (server identity + capabilities) and
    // returns true to accept it. Useful for "pick the engine whose embed
    // dimension is 768" or version pinning.
    [[nodiscard]] Result<Client>
    select_if(const std::function<bool(const Client&)>& pred) const {
        Error last{errc::BackendUnavailable, "no engine satisfied the predicate"};
        for (const auto* s : by_priority()) {
            auto c = s->open();
            if (!c) { last = c.error(); continue; }
            if (pred(*c)) return c;
        }
        return std::unexpected(last);
    }

private:
    // Candidate specs sorted by descending priority (stable for equal priority).
    [[nodiscard]] std::vector<const EngineSpec*> by_priority() const {
        std::vector<const EngineSpec*> v;
        v.reserve(specs_.size());
        for (const auto& s : specs_) v.push_back(&s);
        std::stable_sort(v.begin(), v.end(),
                         [](const EngineSpec* a, const EngineSpec* b) { return a->priority > b->priority; });
        return v;
    }

    std::vector<EngineSpec> specs_;
};

} // namespace rcp
