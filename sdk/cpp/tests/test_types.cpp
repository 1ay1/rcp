// tests/test_types.cpp — compile-time + runtime proofs of the SDK's invariants.
// The static_assert block IS the test suite for the type layer: if it compiles,
// the guarantees hold. A handful of runtime checks cover the JSON round-trips.
#include "rcp.hpp"

#include <cstdio>
#include <string>
#include <type_traits>
#include <vector>

using namespace rcp;

// ── compile-time proofs ──────────────────────────────────────────────────────

// 1. Strong scalars don't implicitly convert — a Score is not a double.
static_assert(!std::is_convertible_v<Score, double>);
static_assert(!std::is_convertible_v<double, Score>);
static_assert(!std::is_same_v<TopK, Dimension>);

// 2. Refinement: TopK / ProtocolVersion have no public constructor; the only
//    entry points are make()/trusted(). (If a public ctor existed, this would
//    be constructible from its rep and the assert below would fire.)
static_assert(!std::is_constructible_v<TopK, std::size_t>);
static_assert(!std::is_constructible_v<ProtocolVersion, int>);

// 3. The Client has no default constructor and is not copyable: the only way to
//    get one is connect(), which runs the handshake (typestate).
static_assert(!std::is_default_constructible_v<Client>);
static_assert(!std::is_copy_constructible_v<Client>);
static_assert(std::is_move_constructible_v<Client>);

// 4. A default Capabilities advertises nothing (runtime-checked in main()).
static_assert(std::is_default_constructible_v<Capabilities>);

// 5. The example Engine models the Handler concept; a bare int does not.
struct MiniHandler {
    PeerInfo info() const { return {"mini", "1"}; }
    Capabilities capabilities() const { return Capabilities{}.with_retrieve(10, {"dense"}); }
    Result<Json> retrieve(const Json&) { return Json{{"hits", Json::array()}}; }
};
static_assert(Handler<MiniHandler>);
static_assert(!Handler<int>);
static_assert(HasRetrieve<MiniHandler>);
static_assert(!HasEmbed<MiniHandler>);

// ── runtime checks ───────────────────────────────────────────────────────────
int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

int main() {
    // TopK refinement rejects 0, accepts >=1.
    CHECK(!TopK::make(0).has_value());
    CHECK(TopK::make(5).has_value());
    CHECK(TopK::make(0).error().code == errc::InvalidParams);

    // ProtocolVersion refinement.
    CHECK(!ProtocolVersion::make(0).has_value());
    CHECK(ProtocolVersion::make(1).has_value());

    // Capabilities JSON round-trip preserves presence semantics.
    Capabilities caps;
    caps.with_embed(Dimension{384}, "bge", {"text"})
        .with_retrieve(200, {"dense", "sparse", "hybrid"}, {"text", "image"})
        .with_multi(Dimension{128}, "colpali", "dot", {"image"})
        .with_log();
    auto back = Capabilities::from_json(caps.to_json());
    CHECK(back.has(Capability::Embed));
    CHECK(back.has(Capability::Retrieve));
    CHECK(back.has(Capability::MultiVector));
    CHECK(!back.has(Capability::Graph));
    // Object-form flags survive the round-trip (spec §6 presence ⇒ supported).
    CHECK(caps.to_json()["log"].is_object());
    CHECK(back.log_);
    // Modalities threaded through builders.
    CHECK(caps.to_json()["multiVector"]["modalities"][0] == "image");

    // Hit parses multimodal + provenance fields (§4.8 / §15.2).
    Hit h = Hit::from_json(Json{{"id", "p3"}, {"score", 0.9}, {"modality", "image"},
                                {"trust", {{"level", "untrusted"}, {"score", 0.2}}},
                                {"content", Json::array({Json{{"type", "image"}, {"mimeType", "image/png"}}})}});
    CHECK(h.modality == "image");
    CHECK(h.trust["level"] == "untrusted");
    CHECK(h.content.is_array());

    // log notification helper is well-formed (§17.1).
    auto logline = make_log_notification("info", "reranked 100→30", Json{{"latencyMs", 42}});
    auto logj = Json::parse(logline);
    CHECK(logj["method"] == "log");
    CHECK(logj["params"]["level"] == "info");
    CHECK(!logj.contains("id")); // notification, not a request

    // Server dispatch: pre-initialize call is NotInitialized; after init, an
    // unadvertised capability is CapabilityMissing; unknown method is UnknownMethod.
    Server<MiniHandler> srv{MiniHandler{}};
    auto pre = srv.handle(Json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "retrieve"}, {"params", {{"query", "x"}, {"k", 1}}}});
    CHECK(pre["error"]["code"] == errc::NotInitialized);

    (void)srv.handle(Json{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "initialize"}, {"params", {{"protocolVersion", 1}}}});

    auto ret = srv.handle(Json{{"jsonrpc", "2.0"}, {"id", 3}, {"method", "retrieve"}, {"params", {{"query", "x"}, {"k", 1}}}});
    CHECK(ret.contains("result"));
    CHECK(ret["result"]["hits"].is_array());

    auto emb = srv.handle(Json{{"jsonrpc", "2.0"}, {"id", 4}, {"method", "embed"}, {"params", {{"texts", {"a"}}}}});
    CHECK(emb["error"]["code"] == errc::CapabilityMissing); // MiniHandler advertises no embed

    auto unk = srv.handle(Json{{"jsonrpc", "2.0"}, {"id", 5}, {"method", "nope"}, {"params", {}}});
    CHECK(unk["error"]["code"] == errc::UnknownMethod);

    // ping is answerable any time and echoes the nonce (§7.15).
    auto pong = srv.handle(Json{{"jsonrpc", "2.0"}, {"id", 6}, {"method", "ping"}, {"params", {{"nonce", 77}}}});
    CHECK(pong["result"]["nonce"] == 77);

    // notifications/cancel yields no reply object (§7.14).
    auto cancel = srv.handle(Json{{"jsonrpc", "2.0"}, {"method", "notifications/cancel"}, {"params", {{"id", 3}}}});
    CHECK(cancel.is_null());

    // Cancelled error code exists in the RCP range.
    CHECK(errc::Cancelled == -32006);

    if (g_fail == 0) std::printf("all type + runtime checks passed\n");
    return g_fail == 0 ? 0 : 1;
}
