#pragma once
// rcp/transport.hpp — the request/reply seam (subprocess pipe or HTTP).
//
// A Transport is a total function Json -> Result<Json>. The client is written
// once against this interface; a test can inject an in-memory fake.
//
// Both concrete transports are fully cross-platform: every OS call goes through
// rcp/platform.hpp, which uses fork/exec + BSD sockets on POSIX and
// CreateProcess + Winsock2 on Windows. There are no #ifdefs in this file and no
// emulation anywhere — each platform runs its own native path at native speed.

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "rcp/platform.hpp"
#include "rcp/types.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace rcp {

// Ensure writing to a dead peer fails the call instead of killing the process
// (SIGPIPE on POSIX), and that Winsock is started on Windows. One name, because
// it is one intent; see platform.hpp for why the mechanisms differ.
inline void ignore_sigpipe() { plat::init_sockets(); }

struct Transport {
    virtual ~Transport() = default;
    // Send one JSON-RPC request object, return the reply object.
    [[nodiscard]] virtual Result<Json> call(const Json& request) = 0;
    virtual void close() {}
};

// ── Subprocess transport: newline-delimited JSON over a child's stdio. ───────
class StdioTransport final : public Transport {
public:
    [[nodiscard]] static Result<std::unique_ptr<StdioTransport>>
    spawn(const std::vector<std::string>& argv) {
        if (argv.empty())
            return fail<std::unique_ptr<StdioTransport>>(errc::InvalidParams, "empty argv");
        plat::Child c = plat::spawn_child(argv);
        if (!c.valid())
            return fail<std::unique_ptr<StdioTransport>>(errc::BackendUnavailable,
                                                         "failed to spawn '" + argv[0] + "'");
        auto t = std::unique_ptr<StdioTransport>(new StdioTransport());
        t->child_ = c;
        return t;
    }

    ~StdioTransport() override { close(); }

    [[nodiscard]] Result<Json> call(const Json& request) override {
        std::string line = request.dump();
        line.push_back('\n');
        if (!plat::fd_write_all(child_.stdin_fd, line))
            return fail<Json>(errc::BackendUnavailable, "write to server failed");
        for (;;) {
            auto ln = read_line();
            if (!ln) return std::unexpected(ln.error());
            Json reply;
            try { reply = Json::parse(*ln); }
            catch (const std::exception& e) { return fail<Json>(errc::ParseError, e.what()); }
            // Skip any server-initiated notification (progress, log, or a future
            // notifications/* frame): a response has no "method". And ignore a
            // stray reply whose id does not match this request (spec §4.7).
            if (reply.contains("method")) continue;
            if (request.contains("id") && reply.contains("id") && reply["id"] != request["id"]) continue;
            return reply;
        }
    }

    void close() override { plat::reap_child(child_); }

private:
    StdioTransport() = default;
    Result<std::string> read_line() {
        for (;;) {
            if (auto nl = buf_.find('\n'); nl != std::string::npos) {
                std::string line = buf_.substr(0, nl);
                buf_.erase(0, nl + 1);
                // Tolerate a CRLF-framed peer: a Windows child that writes in
                // text mode, or any peer following the more liberal framing.
                if (!line.empty() && line.back() == '\r') line.pop_back();
                return line;
            }
            char tmp[4096];
            const long r = plat::fd_read(child_.stdout_fd, tmp, sizeof tmp);
            if (r < 0) return fail<std::string>(errc::BackendUnavailable, "read failed");
            if (r == 0) return fail<std::string>(errc::BackendUnavailable, "server closed the connection");
            buf_.append(tmp, static_cast<std::size_t>(r));
        }
    }
    plat::Child child_;
    std::string buf_;
};

// ── HTTP transport: POST <base>/<method>. ──────────────────────────────
class HttpTransport final : public Transport {
public:
    explicit HttpTransport(std::string base_url) : base_(std::move(base_url)) {
        plat::init_sockets();
        if (!base_.empty() && base_.back() == '/') base_.pop_back();
    }
    [[nodiscard]] Result<Json> call(const Json& request) override {
        std::string method = request.value("method", std::string{});
        std::string url = base_ + "/" + method;
        auto rest = url.find("http://");
        if (rest == std::string::npos) return fail<Json>(errc::InvalidParams, "only http:// supported");
        std::string a = url.substr(rest + 7);
        std::string host = a; std::string path = "/";
        if (auto s = a.find('/'); s != std::string::npos) { host = a.substr(0, s); path = a.substr(s); }
        int port = 80;
        if (auto c = host.find(':'); c != std::string::npos) { port = std::stoi(host.substr(c + 1)); host = host.substr(0, c); }

        // getaddrinfo under the hood: IPv6-capable and thread-safe, unlike the
        // gethostbyname() this used to call.
        plat::socket_t fd = plat::connect_tcp(host, static_cast<std::uint16_t>(port));
        if (!plat::valid(fd)) return fail<Json>(errc::BackendUnavailable, "connect to " + host + " failed");

        std::string body = request.dump();
        std::string req = "POST " + path + " HTTP/1.1\r\nHost: " + host +
                          "\r\nContent-Type: application/json\r\nContent-Length: " +
                          std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        if (!plat::send_all(fd, req)) {
            plat::close_socket(fd);
            return fail<Json>(errc::BackendUnavailable, "write failed");
        }
        std::string resp;
        char tmp[4096];
        for (;;) {
            const long r = plat::sock_recv(fd, tmp, sizeof tmp);
            if (r < 0) { if (plat::interrupted()) continue; break; }
            if (r == 0) break;
            resp.append(tmp, static_cast<std::size_t>(r));
        }
        plat::close_socket(fd);
        // Parse the HTTP status line; non-2xx is a transport failure (spec §5.2).
        int status = 0;
        if (resp.compare(0, 5, "HTTP/") == 0) {
            if (auto sp = resp.find(' '); sp != std::string::npos) status = std::atoi(resp.c_str() + sp + 1);
        }
        auto sep = resp.find("\r\n\r\n");
        std::string b = sep != std::string::npos ? resp.substr(sep + 4) : resp;
        if (status == 429) return fail<Json>(errc::RateLimited, "HTTP 429");
        if (status == 503) return fail<Json>(errc::BackendUnavailable, "HTTP 503");
        if (status && (status < 200 || status >= 300) && b.find_first_not_of(" \r\n\t") == std::string::npos)
            return fail<Json>(errc::BackendUnavailable, "HTTP " + std::to_string(status));
        try { return Json::parse(b); }
        catch (const std::exception& e) { return fail<Json>(errc::ParseError, e.what()); }
    }
private:
    std::string base_;
};

} // namespace rcp
