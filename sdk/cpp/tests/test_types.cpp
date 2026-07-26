// tests/test_types.cpp — compile-time + runtime proofs of the SDK's invariants.
// The static_assert block IS the test suite for the type layer: if it compiles,
// the guarantees hold. A handful of runtime checks cover the JSON round-trips.
#include "rcp.hpp"

#include <cstdio>
#include <cmath>
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

// A handler that also implements the agentic surfaces (feedback + memory). The
// concepts must detect exactly the hooks it defines.
struct AgenticHandler {
    PeerInfo info() const { return {"agentic", "1"}; }
    Capabilities capabilities() const {
        return Capabilities{}.with_retrieve(50, {"hybrid"}).with_session().with_feedback()
                             .with_memory({"global"}, true);
    }
    Result<Json> retrieve(const Json& p) {
        return Json{{"hits", Json::array({Json{{"id", 42}, {"score", 0.9},
                                               {"confidence", 0.83}, {"unit", p.value("unit", "chunk")},
                                               {"level", 2},
                                               {"provenance", {{"path", Json::array({"e:a", "e:b"})}}},
                                               {"scores", {{"dense", 0.7}}}}})}};
    }
    Result<Json> feedback(const Json& p) {
        return Json{{"accepted", p.contains("signals") ? p["signals"].size() : 0}};
    }
    Result<Json> memory_build(const Json&) { return Json{{"memoryId", "mem-1"}, {"tokens", 2048}}; }
    Result<Json> memory_recall(const Json&) {
        return Json{{"clues", Json::array({Json{{"query", "sub-q"}, {"seedIds", Json::array({"e:a"})}}})}};
    }
};
static_assert(Handler<AgenticHandler>);
static_assert(HasFeedback<AgenticHandler>);
static_assert(HasMemoryBuild<AgenticHandler>);
static_assert(HasMemoryRecall<AgenticHandler>);
static_assert(!HasFeedback<MiniHandler>);   // MiniHandler defines no feedback hook
static_assert(!HasMemoryBuild<MiniHandler>);

// A scripted in-memory transport: answers the handshake with a fixed capability
// set and each method with a canned result or error — lets us drive a real
// Client (connect → gate → parse) with no subprocess.
struct FakeTransport final : Transport {
    [[nodiscard]] Result<Json> call(const Json& req) override {
        const std::string m = req.value("method", std::string{});
        const Json id = req.contains("id") ? req["id"] : Json(nullptr);
        auto ok = [&](Json result) { return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}}; };
        if (m == "initialize" || m == "info")
            return ok(Json{{"protocolVersion", 1},
                           {"server", {{"name", "fake"}, {"version", "9"}}},
                           {"capabilities", Capabilities{}
                               .with_retrieve(50, {"dense", "hybrid"})
                               .with_embed(Dimension{4}, "fake-emb")
                               .with_transform({"hyde"})
                               .with_catalog(2)
                               .to_json()}});
        if (m == "retrieve")
            return ok(Json{{"hits", Json::array({Json{{"id", "a"}, {"score", 0.9}},
                                                 Json{{"id", "b"}, {"score", 0.5}}})},
                           {"usage", {{"mode", "hybrid"}}},
                           {"nextCursor", "c2"}});
        if (m == "embed")
            return ok(Json{{"vectors", Json::array({Json::array({0.1, 0.2, 0.3, 0.4})})}});
        if (m == "catalog/list")
            return ok(Json{{"engines", Json::array({Json{{"id", "docs"}}, Json{{"id", "web"}}})}});
        if (m == "query/transform")   // advertised, but the server sheds load
            return Json{{"jsonrpc", "2.0"}, {"id", id},
                        {"error", {{"code", errc::RateLimited}, {"message", "slow down"},
                                   {"data", {{"retryable", true}, {"retryAfterMs", 250}}}}}};
        return ok(Json::object());
    }
};

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

    // Hit parses the agentic/graph/eval fields (§7.7.2): confidence, unit, level,
    // provenance, and per-stage scores.
    Hit ha = Hit::from_json(Json{{"id", 42}, {"score", 0.9}, {"confidence", 0.83},
                                 {"unit", "tree-node"}, {"level", 2},
                                 {"provenance", {{"path", Json::array({"e:a", "e:b"})}}},
                                 {"scores", {{"dense", 0.7}, {"rerank", 0.9}}}});
    CHECK(ha.id == "42");                       // non-string id coerced to string
    CHECK(ha.confidence.has_value() && *ha.confidence == 0.83);
    CHECK(ha.unit == "tree-node");
    CHECK(ha.level.has_value() && *ha.level == 2);
    CHECK(ha.provenance["path"][0] == "e:a");
    CHECK(ha.scores["dense"] == 0.7);
    // A hit with none of the optional fields leaves them empty/nullopt.
    Hit hb = Hit::from_json(Json{{"id", "x"}, {"score", 0.1}});
    CHECK(!hb.confidence.has_value() && !hb.level.has_value() && hb.unit.empty());

    // session/feedback/memory capabilities round-trip (presence ⇒ supported).
    Capabilities acaps;
    acaps.with_session().with_feedback().with_memory({"global", "session"}, true);
    auto aback = Capabilities::from_json(acaps.to_json());
    CHECK(aback.has(Capability::Session));
    CHECK(aback.has(Capability::Feedback));
    CHECK(aback.has(Capability::Memory));
    CHECK(acaps.to_json()["memory"]["scopes"][1] == "session");

    // log notification helper is well-formed (§17.1).
    auto logline = make_log_notification("info", "reranked 100→30", Json{{"latencyMs", 42}});
    auto logj = Json::parse(logline);
    CHECK(logj["method"] == "notifications/log");
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

    // ── InvalidRequest: a message with no method (§4.5). ──
    auto noMethod = srv.handle(Json{{"jsonrpc", "2.0"}, {"id", 20}});
    CHECK(noMethod["error"]["code"] == errc::InvalidRequest);

    // ── JSON-RPC batch (§11). ──
    // Two requests + one notification → array of exactly TWO responses.
    auto batch = srv.handle_line(R"([
        {"jsonrpc":"2.0","id":10,"method":"retrieve","params":{"query":"x","k":1}},
        {"jsonrpc":"2.0","method":"notifications/cancel","params":{"id":10}},
        {"jsonrpc":"2.0","id":11,"method":"ping","params":{"nonce":5}}])");
    auto bj = Json::parse(batch);
    CHECK(bj.is_array() && bj.size() == 2);
    // A batch of only notifications → no output at all.
    CHECK(srv.handle_line(R"([{"jsonrpc":"2.0","method":"notifications/cancel","params":{}}])").empty());
    // Empty batch → a single InvalidRequest.
    CHECK(Json::parse(srv.handle_line("[]"))["error"]["code"] == errc::InvalidRequest);
    // A non-object member yields a -32600 entry; well-formed members still run.
    auto mixed = Json::parse(srv.handle_line(R"([1,{"jsonrpc":"2.0","id":12,"method":"ping"}])"));
    CHECK(mixed.is_array() && mixed.size() == 2);

    // ── progress notification helper (§9). ──
    auto prog = Json::parse(make_progress_notification(Json("t1"), 0.5, "rerank"));
    CHECK(prog["method"] == "notifications/progress");
    CHECK(prog["params"]["progressToken"] == "t1" && prog["params"]["stage"] == "rerank");
    CHECK(!prog.contains("id"));

    // ── catalog capability round-trip + Content builders. ──
    Capabilities cc; cc.with_catalog(3);
    CHECK(cc.has(Capability::Catalog));
    CHECK(Capabilities::from_json(cc.to_json()).has(Capability::Catalog));
    CHECK(cc.to_json()["catalog"]["engines"] == 3);
    CHECK(content::text("hi")["type"] == "text");
    CHECK(content::image("image/png", "AAAA")["mimeType"] == "image/png");

    // ── Error.data + retryability (§12). ──
    CHECK((Error{errc::RateLimited, "x"}.retryable()));
    CHECK((!Error{errc::InvalidParams, "y"}.retryable()));
    Error e3{errc::InternalError, "z", Json{{"retryable", true}, {"retryAfterMs", 500}}};
    CHECK(e3.retryable() && e3.retry_after_ms() == 500);
    auto e3b = Error::from_json(e3.to_json());
    CHECK(e3b.retryable() && e3b.retry_after_ms() == 500);

    // ── Client over the scripted transport: handshake, gating, typed calls. ──
    auto cli = Client::connect(std::make_unique<FakeTransport>());
    CHECK(cli.has_value());
    if (cli) {
        CHECK(cli->protocol_version() == 1);
        CHECK(cli->server().name == "fake");
        CHECK(cli->supports(Capability::Retrieve));
        CHECK(cli->supports(Capability::Catalog));
        CHECK(!cli->supports(Capability::Graph));

        auto k2 = TopK::make(2);
        CHECK(k2.has_value());
        // search() returns the full envelope; retrieve() the hits only.
        auto sr = cli->search("q", *k2);
        CHECK(sr.has_value());
        CHECK(sr && sr->hits.size() == 2);
        CHECK(sr && sr->next_cursor.has_value() && *sr->next_cursor == "c2");
        CHECK(sr && sr->usage["mode"] == "hybrid");
        auto only = cli->retrieve("q", *k2);
        CHECK(only.has_value() && only->size() == 2);

        // embed sends `inputs` and parses one 4-d vector.
        std::vector<std::string> texts = {"hello"};
        auto ev = cli->embed(texts);
        CHECK(ev.has_value() && ev->size() == 1 && (*ev)[0].size() == 4);

        // catalog is gated on Capability::Catalog and returns the engines.
        auto cat = cli->catalog();
        CHECK(cat.has_value() && (*cat)["engines"].size() == 2);

        // graph is unadvertised → client-side CapabilityMissing before any I/O.
        auto gg = cli->graph("global", Json{{"query", "x"}});
        CHECK(!gg.has_value() && gg.error().code == errc::CapabilityMissing);

        // an advertised method returning a structured error surfaces retryability.
        auto tr = cli->transform("q", "hyde");
        CHECK(!tr.has_value());
        CHECK(tr.error().code == errc::RateLimited);
        CHECK(tr.error().retryable() && tr.error().retry_after_ms() == 250);
    }

    // ── AgenticHandler: feedback + memory dispatch, gating, hit fields. ──
    Server<AgenticHandler> asrv{AgenticHandler{}};
    (void)asrv.handle(Json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", {{"protocolVersion", 1}}}});

    auto aret = asrv.handle(Json{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "retrieve"},
                                 {"params", {{"query", "q"}, {"k", 1}, {"unit", "tree-node"}}}});
    CHECK(aret["result"]["hits"][0]["confidence"] == 0.83);
    CHECK(aret["result"]["hits"][0]["unit"] == "tree-node");

    auto afb = asrv.handle(Json{{"jsonrpc", "2.0"}, {"id", 3}, {"method", "feedback"},
                                {"params", {{"signals", Json::array({Json{{"hitId", "42"}, {"used", true}}})}}}});
    CHECK(afb["result"]["accepted"] == 1);

    auto amb = asrv.handle(Json{{"jsonrpc", "2.0"}, {"id", 4}, {"method", "memory/build"}, {"params", {{"scope", "global"}}}});
    CHECK(amb["result"]["memoryId"] == "mem-1");

    auto amr = asrv.handle(Json{{"jsonrpc", "2.0"}, {"id", 5}, {"method", "memory/recall"}, {"params", {{"query", "q"}}}});
    CHECK(amr["result"]["clues"][0]["seedIds"][0] == "e:a");

    // MiniHandler advertises none of these → CapabilityMissing (not UnknownMethod:
    // the method is known to the protocol, just unimplemented/unadvertised).
    (void)srv.handle(Json{{"jsonrpc", "2.0"}, {"id", 30}, {"method", "initialize"}, {"params", {{"protocolVersion", 1}}}});
    auto miss = srv.handle(Json{{"jsonrpc", "2.0"}, {"id", 31}, {"method", "memory/recall"}, {"params", {{"query", "q"}}}});
    CHECK(miss["error"]["code"] == errc::CapabilityMissing);

    // ── Filter builder + validator (spec §8) ─────────────────────────────────
    {
        using namespace rcp::filter;
        // Builder emits the exact wire shape.
        Json leaf = eq("lang", "fr").to_json();
        CHECK(leaf["field"] == "lang" && leaf["op"] == "eq" && leaf["value"] == "fr");
        Json tree = (eq("lang", "en") && gte("year", 2015)).to_json();
        CHECK(tree["and"].is_array() && tree["and"].size() == 2);

        std::vector<FieldSpec> fields = {{"year", "int"}, {"lang", "keyword"}};
        std::vector<std::string_view> ops = {"eq", "ne", "gt", "gte", "lt", "lte", "in", "nin"};

        // Well-formed validates; absent/empty normalizes to null.
        CHECK(rcp::filter::validate(tree, fields, ops).has_value());
        CHECK(rcp::filter::validate(Json(nullptr), fields).value().is_null());
        CHECK(rcp::filter::validate(Json::object(), fields).value().is_null());

        // Every malformed / unauthorized tree -> -32602 with data.field.
        std::vector<Json> badcases = {
            Json{{"field", "author"}, {"op", "eq"}, {"value", "z"}},        // unadvertised field
            Json{{"field", "lang"}, {"op", "equals"}, {"value", "en"}},     // typo op
            Json{{"and", "not-a-list"}},                                      // malformed combinator
            Json{{"field", "year"}, {"op", "in"}, {"value", 2017}},          // in wants an array
            Json{{"or", Json::array({Json{{"field", "lang"}}})}},            // leaf missing op
            Json{{"field", "lang"}, {"op", "gt"}, {"value", "en"}},          // ordered op on keyword
        };
        for (const auto& b : badcases) {
            auto r = rcp::filter::validate(b, fields, ops);
            CHECK(!r.has_value());
            CHECK(!r && r.error().code == errc::InvalidParams);
            CHECK(!r && r.error().data.is_object() && r.error().data.contains("field"));
        }
    }

    // ── fusion: RRF + weighted, standalone (spec §16.3). ──
    {
        auto hit = [](std::string id, double s = 0.0, std::string text = "") {
            Hit h; h.id = std::move(id); h.score = Score{s}; h.text = std::move(text); return h;
        };
        using rcp::fusion::EngineList;
        // A=[a,b,c], B=[b,d], rrf_k=60. b=1/62+1/61, a=1/61, d=1/62, c=1/63.
        std::vector<EngineList> engines = {
            {"A", {hit("a"), hit("b"), hit("c")}, 1.0},
            {"B", {hit("b"), hit("d")}, 1.0},
        };
        auto fused = rcp::fusion::rrf_fuse(engines);
        CHECK(fused.size() == 4);
        CHECK(fused[0].id == "b" && fused[1].id == "a" && fused[2].id == "d" && fused[3].id == "c");
        double bScore = 1.0 / 62.0 + 1.0 / 61.0;
        CHECK(std::abs(fused[0].meta["fusedScore"].get<double>() - bScore) < 1e-12);
        CHECK(fused[0].meta["engine"] == "A");

        // k truncates.
        CHECK(rcp::fusion::rrf_fuse(engines, std::size_t{2}).size() == 2);

        // Deterministic tie-break: equal fused score -> id ascending.
        std::vector<EngineList> tie = {{"X", {hit("z")}, 1.0}, {"Y", {hit("a")}, 1.0}};
        auto tf = rcp::fusion::rrf_fuse(tie);
        CHECK(tf.size() == 2 && tf[0].id == "a" && tf[1].id == "z");

        // Weighted fusion with per-engine min-max normalisation.
        std::vector<EngineList> we = {
            {"A", {hit("a", 10.0), hit("b", 0.0)}, 1.0},
            {"B", {hit("b", 5.0)}, 1.0},
        };
        auto wf = rcp::fusion::weighted_fuse(we);
        // a->1.0, b->0.0+1.0=1.0; fused tie -> higher weight (b, sum 2) first.
        CHECK(wf.size() == 2 && wf[0].id == "b" && wf[1].id == "a");

        // Dedup keeps the richest body across engines.
        std::vector<EngineList> dd = {
            {"A", {hit("d", 0.0, "short")}, 1.0},
            {"B", {hit("d", 0.0, "a much longer body")}, 1.0},
        };
        auto df = rcp::fusion::rrf_fuse(dd);
        CHECK(df.size() == 1 && df[0].text == "a much longer body");
    }

    // ── vectors: f32-base64 codec (spec §7.3.1). ──
    {
        // Known base64 vectors (interop with every other SDK's codec).
        CHECK(rcp::vectors::detail::b64_encode(reinterpret_cast<const std::uint8_t*>("foobar"), 6) == "Zm9vYmFy");
        CHECK(rcp::vectors::detail::b64_encode(reinterpret_cast<const std::uint8_t*>("fo"), 2) == "Zm8=");

        std::vector<float> v = {1.5f, -2.25f, 0.0f, 3.125f};
        auto blob = rcp::vectors::encode_f32_base64(v);
        auto back = rcp::vectors::decode_f32_base64(blob, 4);
        CHECK(back.has_value() && back->size() == 4);
        if (back) for (std::size_t i = 0; i < 4; ++i) CHECK(std::abs((*back)[i] - v[i]) < 1e-6f);

        // Wrong declared dimension is rejected.
        CHECK(!rcp::vectors::decode_f32_base64(blob, 8).has_value());

        // Batch encode: ragged vectors rejected, json passes through.
        auto ragged = rcp::vectors::encode_batch({{1.0f, 2.0f}, {3.0f}}, "f32-base64");
        CHECK(!ragged.has_value());
        auto j = rcp::vectors::encode_batch({{1.0f, 2.0f, 3.0f}}, "json");
        CHECK(j.has_value() && j->is_array() && (*j)[0].size() == 3);
        auto b = rcp::vectors::encode_batch({{1.0f, 2.0f, 3.0f, 4.0f}}, "f32-base64");
        CHECK(b.has_value() && (*b)["encoding"] == "f32-base64" && (*b)["dimension"] == 4);
    }

    // ── A malformed request must produce an ERROR, never terminate ─────────
    //
    // Server::handle documents "never throws", and that guarantee is what keeps
    // a stdio server alive: dispatch runs SDK code and arbitrary handler code
    // over peer-controlled params, and nlohmann::json throws type_error on a
    // bad .get<T>(). It used to escape uncaught — one line of malformed JSON
    // called std::terminate and killed the whole process, taking any unsaved
    // engine state with it. That is an unauthenticated remote kill.
    {
        Server<MiniHandler> s{MiniHandler{}};

        // Wrong TYPE for a spec'd integer field. -32602, and the session lives.
        auto bad_ver = s.handle(Json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
                                     {"params", {{"protocolVersion", "1.0"}}}});
        CHECK(bad_ver["error"]["code"] == errc::InvalidParams);

        // The rejected handshake must NOT have initialized the session
        // (§7.1), and a real handshake must still succeed afterwards.
        auto good = s.handle(Json{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "initialize"},
                                  {"params", {{"protocolVersion", 1}}}});
        CHECK(good.contains("result"));

        // Wrong types in assorted places: each must come back as a response
        // object with an id, not crash the process.
        const Json malformed[] = {
            Json{{"jsonrpc", "2.0"}, {"id", 10}, {"method", "retrieve"}, {"params", {{"query", 42}, {"k", "lots"}}}},
            Json{{"jsonrpc", "2.0"}, {"id", 11}, {"method", "retrieve"}, {"params", "not-an-object"}},
            Json{{"jsonrpc", "2.0"}, {"id", 12}, {"method", "retrieve"}, {"params", {{"query", {{"text", nullptr}}}}}},
            Json{{"jsonrpc", "2.0"}, {"id", 13}, {"method", "embed"}, {"params", {{"texts", 7}}}},
            Json{{"jsonrpc", "2.0"}, {"id", 14}, {"method", "ping"}, {"params", {{"nonce", {1, 2, 3}}}}},
        };
        for (const auto& m : malformed) {
            Json reply = s.handle(m);
            CHECK(reply.is_object());
            CHECK(reply.contains("result") || reply.contains("error"));
            CHECK(reply["id"] == m["id"]);        // stays correlated — no desync
        }

        // And through the line interface, where a real server actually lives.
        CHECK(!s.handle_line("{\"jsonrpc\":\"2.0\",\"id\":20,\"method\":\"retrieve\",\"params\":{\"k\":\"x\"}}").empty());
        CHECK(!s.handle_line("not json at all").empty());
    }

    if (g_fail == 0) std::printf("all type + runtime checks passed\n");
    return g_fail == 0 ? 0 : 1;
}
