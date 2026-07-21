#pragma once
// rcp/transport.hpp — the request/reply seam (subprocess pipe or HTTP).
//
// A Transport is a total function Json -> Result<Json>. Concrete transports are
// POSIX (fork/exec pipe, minimal HTTP/1.1). The client is written once against
// this interface; a test can inject an in-memory fake.

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "rcp/types.hpp"

#include <csignal>
#include <cstring>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace rcp {

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
        if (argv.empty()) return fail<std::unique_ptr<StdioTransport>>(errc::InvalidParams, "empty argv");
        int in_pipe[2], out_pipe[2];
        if (::pipe(in_pipe) != 0 || ::pipe(out_pipe) != 0)
            return fail<std::unique_ptr<StdioTransport>>(errc::BackendUnavailable, "pipe() failed");
        pid_t pid = ::fork();
        if (pid < 0) return fail<std::unique_ptr<StdioTransport>>(errc::BackendUnavailable, "fork() failed");
        if (pid == 0) {
            ::dup2(in_pipe[0], STDIN_FILENO);
            ::dup2(out_pipe[1], STDOUT_FILENO);
            ::close(in_pipe[0]); ::close(in_pipe[1]); ::close(out_pipe[0]); ::close(out_pipe[1]);
            std::vector<char*> args;
            for (auto& a : const_cast<std::vector<std::string>&>(argv)) args.push_back(a.data());
            args.push_back(nullptr);
            ::execvp(args[0], args.data());
            ::_exit(127);
        }
        ::close(in_pipe[0]); ::close(out_pipe[1]);
        auto t = std::unique_ptr<StdioTransport>(new StdioTransport());
        t->pid_ = pid; t->to_ = in_pipe[1]; t->from_ = out_pipe[0];
        return t;
    }

    ~StdioTransport() override { close(); }

    [[nodiscard]] Result<Json> call(const Json& request) override {
        std::string line = request.dump();
        line.push_back('\n');
        if (!write_all(line)) return fail<Json>(errc::BackendUnavailable, "write to server failed");
        for (;;) {
            auto ln = read_line();
            if (!ln) return std::unexpected(ln.error());
            Json reply;
            try { reply = Json::parse(*ln); }
            catch (const std::exception& e) { return fail<Json>(errc::ParseError, e.what()); }
            // Skip progress notifications for the synchronous client.
            if (reply.value("method", std::string{}) == "notifications/progress") continue;
            return reply;
        }
    }

    void close() override {
        if (to_ >= 0) { ::close(to_); to_ = -1; }
        if (from_ >= 0) { ::close(from_); from_ = -1; }
        if (pid_ > 0) { int st; ::kill(pid_, SIGTERM); ::waitpid(pid_, &st, 0); pid_ = -1; }
    }

private:
    StdioTransport() = default;
    bool write_all(std::string_view s) {
        const char* p = s.data(); size_t n = s.size();
        while (n) { ssize_t w = ::write(to_, p, n); if (w < 0) { if (errno == EINTR) continue; return false; } p += w; n -= (size_t)w; }
        return true;
    }
    Result<std::string> read_line() {
        for (;;) {
            if (auto nl = buf_.find('\n'); nl != std::string::npos) {
                std::string line = buf_.substr(0, nl); buf_.erase(0, nl + 1); return line;
            }
            char tmp[4096];
            ssize_t r = ::read(from_, tmp, sizeof tmp);
            if (r < 0) { if (errno == EINTR) continue; return fail<std::string>(errc::BackendUnavailable, "read failed"); }
            if (r == 0) return fail<std::string>(errc::BackendUnavailable, "server closed the connection");
            buf_.append(tmp, (size_t)r);
        }
    }
    pid_t pid_ = -1; int to_ = -1, from_ = -1; std::string buf_;
};

// ── HTTP transport: POST <base>/<method>. ───────────────────────────────────
class HttpTransport final : public Transport {
public:
    explicit HttpTransport(std::string base_url) : base_(std::move(base_url)) {
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

        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return fail<Json>(errc::BackendUnavailable, "socket() failed");
        sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons((uint16_t)port);
        if (hostent* he = ::gethostbyname(host.c_str()); he && he->h_addr_list[0])
            std::memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
        else addr.sin_addr.s_addr = ::inet_addr(host.c_str());
        if (::connect(fd, (sockaddr*)&addr, sizeof addr) != 0) { ::close(fd); return fail<Json>(errc::BackendUnavailable, "connect failed"); }

        std::string body = request.dump();
        std::string req = "POST " + path + " HTTP/1.1\r\nHost: " + host +
                          "\r\nContent-Type: application/json\r\nContent-Length: " +
                          std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        (void)!::write(fd, req.data(), req.size());
        std::string resp; char tmp[4096]; ssize_t r;
        while ((r = ::read(fd, tmp, sizeof tmp)) > 0) resp.append(tmp, (size_t)r);
        ::close(fd);
        auto sep = resp.find("\r\n\r\n");
        std::string b = sep != std::string::npos ? resp.substr(sep + 4) : resp;
        try { return Json::parse(b); }
        catch (const std::exception& e) { return fail<Json>(errc::ParseError, e.what()); }
    }
private:
    std::string base_;
};

} // namespace rcp
