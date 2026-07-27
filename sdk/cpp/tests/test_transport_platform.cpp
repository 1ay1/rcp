// tests/test_transport_platform.cpp — the OS seam, exercised on every platform.
//
// test_types.cpp covers the type/protocol layer. This file exists for the
// property that layer cannot check: that the SOCKET and SUBPROCESS code paths
// actually LINK and RUN on the host we are building for.
//
// That distinction is not academic. The bug this file was written for shipped
// in a header-only SDK whose headers included <netinet/in.h> and called
// fork/execvp: every consumer on Windows failed at the first include, and no
// test caught it because nothing ever compiled the transports outside Linux.
// A test that only constructs types would still pass with the socket code
// completely broken, so each case below forces real OS calls to be emitted and
// then executed.
//
// Every assertion here must hold identically on Linux, macOS and Windows.

#include "rcp/client.hpp"
#include "rcp/server.hpp"
#include "rcp/transport.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

static int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

namespace {

struct Probe {
    rcp::PeerInfo     info()         const { return {"probe", "0"}; }
    rcp::Capabilities capabilities() const { return {}; }
};

// Port 1 is reserved and never listening, so connect() must fail promptly.
// The point is that it fails as a TYPED ERROR: on Windows a missing WSAStartup
// would instead make every socket call fail in a way that used to be reported
// as a generic failure, and on POSIX an unignored SIGPIPE would kill us outright
// rather than return.
void http_transport_reaches_the_network_stack() {
    rcp::HttpTransport http{"http://127.0.0.1:1/"};
    auto r = http.call(nlohmann::json{{"method", "ping"}});
    CHECK(!r.has_value());
    if (!r.has_value())
        CHECK(r.error().code == rcp::errc::BackendUnavailable);
}

// Name resolution must go through getaddrinfo and fail cleanly on a name that
// cannot resolve — not hang, and not crash. ".invalid" is reserved by RFC 2606
// precisely so it can never resolve anywhere.
void http_transport_handles_unresolvable_hosts() {
    rcp::HttpTransport http{"http://nonexistent.invalid:80/"};
    auto r = http.call(nlohmann::json{{"method", "ping"}});
    CHECK(!r.has_value());
}

// Only http:// is supported; anything else is rejected before a socket is made.
void http_transport_rejects_other_schemes() {
    rcp::HttpTransport http{"ftp://example.com/"};
    auto r = http.call(nlohmann::json{{"method", "ping"}});
    CHECK(!r.has_value());
    if (!r.has_value())
        CHECK(r.error().code == rcp::errc::InvalidParams);
}

// Spawning must reject an empty argv before touching the OS.
void stdio_transport_rejects_empty_argv() {
    auto t = rcp::StdioTransport::spawn({});
    CHECK(!t.has_value());
    if (!t.has_value())
        CHECK(t.error().code == rcp::errc::InvalidParams);
}

// A program that does not exist must surface as an error, never a hang or a
// crash. POSIX discovers this after fork (the exec fails in the child, so the
// error appears as EOF on the first call); Windows discovers it in
// CreateProcess and fails the spawn itself. Both shapes are acceptable — what
// matters is that the caller gets a typed failure and the process survives.
void stdio_transport_handles_a_missing_program() {
    auto t = rcp::StdioTransport::spawn({"rcp-no-such-program-9f2c1e"});
    if (!t.has_value()) {
        CHECK(t.error().code == rcp::errc::BackendUnavailable);
    } else {
        auto r = (*t)->call(nlohmann::json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "ping"}});
        CHECK(!r.has_value());
    }
}

// A real child, spoken to over real pipes: the end-to-end proof that the
// subprocess transport works on this OS. The echo peer is written in the
// platform's own shell so no extra runtime is required.
//
// This is also the case that catches text-mode pipe corruption: the protocol is
// newline-delimited, so if the CRT rewrote '\n' as "\r\n" the reply would not
// parse.
void stdio_transport_round_trips_with_a_real_child() {
#if defined(_WIN32)
    // cmd.exe cannot loop-and-echo readably, so answer exactly one request.
    // MSYS2 always provides sh, and the CI image runs these tests under it.
    const std::vector<std::string> argv{
        "sh", "-c",
        "IFS= read -r line; printf '{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"pong\":true}}\\n'"};
#else
    const std::vector<std::string> argv{
        "/bin/sh", "-c",
        "IFS= read -r line; printf '{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"pong\":true}}\\n'"};
#endif
    auto t = rcp::StdioTransport::spawn(argv);
    CHECK(t.has_value());
    if (!t.has_value()) return;

    auto r = (*t)->call(nlohmann::json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "ping"}});
    CHECK(r.has_value());
    if (r.has_value()) {
        CHECK(r->contains("result"));
        CHECK((*r)["result"].value("pong", false));
    }
    (*t)->close();   // must be idempotent with the destructor
}

// The server's framing path, independent of any socket: a well-formed request
// for an unknown method gets a JSON-RPC error object, not a crash.
void server_framing_works() {
    rcp::Server<Probe> srv{Probe{}};
    const std::string reply =
        srv.handle_line(R"({"jsonrpc":"2.0","id":1,"method":"no/such/method"})");
    CHECK(!reply.empty());
    if (!reply.empty()) {
        auto j = nlohmann::json::parse(reply, nullptr, false);
        CHECK(!j.is_discarded());
        CHECK(j.contains("error"));
    }
}

// serve_http() binds and listens, so it must LINK on every platform even though
// a unit test cannot sit in its accept loop. Taking the address of the
// instantiated member forces the linker to resolve bind/listen/accept without
// running them.
void server_http_path_links() {
    using ServeFn = rcp::Result<void> (rcp::Server<Probe>::*)(std::uint16_t);
    ServeFn fn = &rcp::Server<Probe>::serve_http;
    CHECK(fn != nullptr);

    // Escape hatch for manual smoke-testing; never taken in CI.
    if (const char* p = std::getenv("RCP_TEST_SERVE_HTTP"); p != nullptr) {
        rcp::Server<Probe> srv{Probe{}};
        (void)(srv.*fn)(static_cast<std::uint16_t>(std::atoi(p)));
    }
}

} // namespace

int main() {
    http_transport_reaches_the_network_stack();
    http_transport_handles_unresolvable_hosts();
    http_transport_rejects_other_schemes();
    stdio_transport_rejects_empty_argv();
    stdio_transport_handles_a_missing_program();
    stdio_transport_round_trips_with_a_real_child();
    server_framing_works();
    server_http_path_links();

    if (g_failures != 0) {
        std::printf("%d transport/platform check(s) FAILED\n", g_failures);
        return 1;
    }
    std::puts("all transport + platform checks passed");
    return 0;
}
