#pragma once
// rcp/platform.hpp — the OS seam for the RCP C++ SDK.
//
// The SDK is header-only and speaks two OS facilities: TCP sockets (HTTP
// transport / HTTP server) and child processes with pipes (stdio transport).
// Both exist natively on POSIX and on Windows, but under different names and
// with different handle types. This header is the ONE place that difference is
// written down; every other header in the SDK calls the neutral names below and
// contains no #ifdef at all.
//
// The rule this file follows: NATIVE CALLS ON EVERY PLATFORM. Nothing here is
// an emulation layer, a compatibility shim over a slow path, or a "good enough
// on Windows" fallback. On POSIX these are the raw syscalls the SDK always
// used; on Windows they are Winsock2 and CreateProcess, which are exactly what
// a native Windows program would call. The inline wrappers compile away — a
// send() is a send() on both sides, so throughput is identical and the abstraction
// costs zero instructions at runtime.
//
// Why not just "#define socket_close close": because the two platforms disagree
// about more than spelling. Windows sockets are SOCKET (an opaque UINT_PTR, not
// a file descriptor, and not interchangeable with one), errors come from
// WSAGetLastError() rather than errno, and the library needs explicit
// initialisation. Those are semantic differences, and hiding them behind macros
// is how you get a build that compiles and then misbehaves at runtime.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>
#include <fcntl.h>

#if defined(_MSC_VER)
#pragma comment(lib, "ws2_32.lib")
#endif

#else

#include <arpa/inet.h>
#include <csignal>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#endif

#include <cerrno>
#include <cstring>

namespace rcp::plat {

// ── Socket handle ────────────────────────────────────────────────────────────
#if defined(_WIN32)
using socket_t = SOCKET;
inline constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
using socket_t = int;
inline constexpr socket_t kInvalidSocket = -1;
#endif

[[nodiscard]] inline bool valid(socket_t s) noexcept { return s != kInvalidSocket; }

// ── One-time initialisation ──────────────────────────────────────────────────
// Winsock must be started before any socket call; POSIX needs SIGPIPE ignored so
// that writing to a dead peer returns EPIPE instead of killing the process.
// Same intent ("make socket errors surface as return values"), different
// mechanism — so it lives behind one name that every entry point calls.
inline void init_sockets() noexcept {
#if defined(_WIN32)
    [[maybe_unused]] static const bool once = [] {
        WSADATA d{};
        ::WSAStartup(MAKEWORD(2, 2), &d);
        // Deliberately never WSACleanup(): the SDK is header-only and has no
        // shutdown hook, and the OS reclaims the reference at process exit.
        return true;
    }();
#else
    [[maybe_unused]] static const bool once = [] { std::signal(SIGPIPE, SIG_IGN); return true; }();
#endif
}

// ── Socket teardown ──────────────────────────────────────────────────────────
inline void close_socket(socket_t s) noexcept {
    if (!valid(s)) return;
#if defined(_WIN32)
    ::closesocket(s);
#else
    ::close(s);
#endif
}

// Was the last socket operation interrupted and worth retrying verbatim?
// Windows has no EINTR on sockets, so this is simply never true there.
[[nodiscard]] inline bool interrupted() noexcept {
#if defined(_WIN32)
    return ::WSAGetLastError() == WSAEINTR;
#else
    return errno == EINTR;
#endif
}

// ── Blocking send/recv ───────────────────────────────────────────────────────
// send()/recv() exist with identical semantics on both platforms; only the
// length type differs (int on Windows, size_t on POSIX). Returns bytes moved, or
// <0 on error.
[[nodiscard]] inline long sock_send(socket_t s, const char* p, std::size_t n) noexcept {
#if defined(_WIN32)
    return ::send(s, p, static_cast<int>(n), 0);
#else
    return static_cast<long>(::send(s, p, n, 0));
#endif
}

[[nodiscard]] inline long sock_recv(socket_t s, char* p, std::size_t n) noexcept {
#if defined(_WIN32)
    return ::recv(s, p, static_cast<int>(n), 0);
#else
    return static_cast<long>(::recv(s, p, n, 0));
#endif
}

// Write every byte, tolerating partial sends. False on a hard error.
[[nodiscard]] inline bool send_all(socket_t s, std::string_view buf) noexcept {
    const char* p = buf.data();
    std::size_t n = buf.size();
    while (n > 0) {
        const long w = sock_send(s, p, n);
        if (w < 0) {
            if (interrupted()) continue;
            return false;
        }
        if (w == 0) return false;
        p += w;
        n -= static_cast<std::size_t>(w);
    }
    return true;
}

// ── Socket options ───────────────────────────────────────────────────────────
inline void set_reuseaddr(socket_t s) noexcept {
    // On Windows SO_REUSEADDR permits stealing a live socket (it means what
    // POSIX calls SO_REUSEPORT), which is a security problem, not a
    // convenience. The POSIX intent — "rebind while old connections linger in
    // TIME_WAIT" — is the DEFAULT behaviour on Windows, so the correct port of
    // this call is to omit it entirely rather than set the same-named flag.
#if !defined(_WIN32)
    int yes = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
#else
    (void)s;
#endif
}

// Disable Nagle. This is a latency decision, not a portability one, and it
// matters equally on both platforms: RCP is a small-request/small-reply RPC
// protocol, and coalescing a 200-byte JSON frame for 40 ms is pure added
// latency on every single call.
inline void set_nodelay(socket_t s) noexcept {
    int yes = 1;
#if defined(_WIN32)
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof yes);
#else
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof yes);
#endif
}

// ── Name resolution ──────────────────────────────────────────────────────────
// getaddrinfo is the modern, thread-safe, IPv6-capable resolver and is present
// on both platforms (Winsock2 via ws2tcpip.h). It replaces the SDK's old
// gethostbyname(), which is deprecated, not thread-safe, and IPv4-only.
// Connects to the first address that accepts. Returns kInvalidSocket on failure.
[[nodiscard]] inline socket_t connect_tcp(const std::string& host, std::uint16_t port) noexcept {
    init_sockets();
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;      // IPv4 or IPv6, whichever resolves
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string port_s = std::to_string(port);
    if (::getaddrinfo(host.c_str(), port_s.c_str(), &hints, &res) != 0 || res == nullptr)
        return kInvalidSocket;

    socket_t fd = kInvalidSocket;
    for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (!valid(fd)) continue;
        if (::connect(fd, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) {
            set_nodelay(fd);
            break;
        }
        close_socket(fd);
        fd = kInvalidSocket;
    }
    ::freeaddrinfo(res);
    return fd;
}

// ── Listening socket ─────────────────────────────────────────────────────────
// Binds loopback only (the SDK's servers are local by design) and starts
// listening. Returns kInvalidSocket on failure.
[[nodiscard]] inline socket_t listen_tcp(std::uint16_t port, int backlog = 16) noexcept {
    init_sockets();
    socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (!valid(fd)) return kInvalidSocket;
    set_reuseaddr(fd);
    sockaddr_in a{};
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port        = htons(port);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        close_socket(fd);
        return kInvalidSocket;
    }
    if (::listen(fd, backlog) != 0) {
        close_socket(fd);
        return kInvalidSocket;
    }
    return fd;
}

[[nodiscard]] inline socket_t accept_one(socket_t server) noexcept {
    socket_t c = ::accept(server, nullptr, nullptr);
    if (valid(c)) set_nodelay(c);
    return c;
}

// ── Child process with pipes ─────────────────────────────────────────────────
// The stdio transport needs: spawn a program, keep its stdin/stdout as byte
// streams, reap it on close. On Windows the parent ends of the pipes are turned
// into CRT file descriptors with _open_osfhandle, so callers read/write plain
// ints on both platforms.
struct Child {
    int  stdin_fd  = -1;      // parent writes -> child's stdin
    int  stdout_fd = -1;      // child's stdout -> parent reads
    long pid       = -1;      // informational on Windows (process id)
#if defined(_WIN32)
    void* handle = nullptr;   // HANDLE; required to wait/terminate. pid can't do it.
#endif
    [[nodiscard]] bool valid() const noexcept { return stdin_fd >= 0 && stdout_fd >= 0; }
};

#if defined(_WIN32)

// Windows has no argv: a process receives one flat command line that each CRT
// re-splits. Quote per the rules the CRT parser implements so an argument with
// spaces, quotes, or trailing backslashes arrives exactly as it left.
[[nodiscard]] inline std::string quote_arg(const std::string& a) {
    if (!a.empty() && a.find_first_of(" \t\n\v\"") == std::string::npos) return a;
    std::string out = "\"";
    for (auto it = a.begin();; ++it) {
        std::size_t slashes = 0;
        while (it != a.end() && *it == '\\') { ++it; ++slashes; }
        if (it == a.end()) { out.append(slashes * 2, '\\'); break; }
        if (*it == '"') { out.append(slashes * 2 + 1, '\\'); out.push_back('"'); }
        else            { out.append(slashes, '\\');         out.push_back(*it); }
    }
    out.push_back('"');
    return out;
}

#endif

// Spawn `argv` with its stdin/stdout piped back to us; stderr is inherited so
// the child can log to our stderr. Returns an invalid Child on failure.
[[nodiscard]] inline Child spawn_child(const std::vector<std::string>& argv) noexcept {
    Child ch;
    if (argv.empty()) return ch;

#if defined(_WIN32)
    SECURITY_ATTRIBUTES sa{};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE in_r = nullptr, in_w = nullptr, out_r = nullptr, out_w = nullptr;
    if (!::CreatePipe(&in_r, &in_w, &sa, 0)) return ch;
    if (!::CreatePipe(&out_r, &out_w, &sa, 0)) {
        ::CloseHandle(in_r); ::CloseHandle(in_w);
        return ch;
    }
    // Our ends must not leak into the child, or we would never observe EOF.
    ::SetHandleInformation(in_w,  HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);

    std::string cmdline;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i) cmdline.push_back(' ');
        cmdline += quote_arg(argv[i]);
    }
    std::vector<char> mut_cmd(cmdline.begin(), cmdline.end());
    mut_cmd.push_back('\0');   // CreateProcessA may write into this buffer

    STARTUPINFOA si{};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = in_r;
    si.hStdOutput = out_w;
    si.hStdError  = ::GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION pi{};
    const BOOL ok = ::CreateProcessA(nullptr, mut_cmd.data(), nullptr, nullptr,
                                     TRUE, 0, nullptr, nullptr, &si, &pi);
    ::CloseHandle(in_r);
    ::CloseHandle(out_w);
    if (!ok) { ::CloseHandle(in_w); ::CloseHandle(out_r); return ch; }
    ::CloseHandle(pi.hThread);

    // _O_BINARY is load-bearing: the protocol is newline-delimited JSON, and a
    // text-mode fd would rewrite every '\n' as "\r\n" and corrupt every frame.
    const int fd_w = ::_open_osfhandle(reinterpret_cast<intptr_t>(in_w),  _O_BINARY | _O_WRONLY);
    const int fd_r = ::_open_osfhandle(reinterpret_cast<intptr_t>(out_r), _O_BINARY | _O_RDONLY);
    if (fd_w < 0 || fd_r < 0) {
        if (fd_w >= 0) ::_close(fd_w); else ::CloseHandle(in_w);
        if (fd_r >= 0) ::_close(fd_r); else ::CloseHandle(out_r);
        ::TerminateProcess(pi.hProcess, 1);
        ::CloseHandle(pi.hProcess);
        return ch;
    }
    // The fd owns the HANDLE from here: closing the fd closes it exactly once.
    ch.stdin_fd  = fd_w;
    ch.stdout_fd = fd_r;
    ch.pid       = static_cast<long>(pi.dwProcessId);
    ch.handle    = pi.hProcess;
    return ch;

#else
    init_sockets();   // SIGPIPE: a write to a dead child must not kill us
    int in_pipe[2], out_pipe[2];
    if (::pipe(in_pipe) != 0) return ch;
    if (::pipe(out_pipe) != 0) { ::close(in_pipe[0]); ::close(in_pipe[1]); return ch; }

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(in_pipe[0]); ::close(in_pipe[1]);
        ::close(out_pipe[0]); ::close(out_pipe[1]);
        return ch;
    }
    if (pid == 0) {
        ::dup2(in_pipe[0], STDIN_FILENO);
        ::dup2(out_pipe[1], STDOUT_FILENO);
        ::close(in_pipe[0]); ::close(in_pipe[1]);
        ::close(out_pipe[0]); ::close(out_pipe[1]);
        std::vector<char*> args;
        args.reserve(argv.size() + 1);
        for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        ::execvp(args[0], args.data());
        ::_exit(127);
    }
    ::close(in_pipe[0]);
    ::close(out_pipe[1]);
    ch.stdin_fd  = in_pipe[1];
    ch.stdout_fd = out_pipe[0];
    ch.pid       = static_cast<long>(pid);
    return ch;
#endif
}

// Close the pipes (EOF on the child's stdin is the polite exit request), then
// reap. Idempotent.
inline void reap_child(Child& ch) noexcept {
#if defined(_WIN32)
    if (ch.stdin_fd  >= 0) { ::_close(ch.stdin_fd);  ch.stdin_fd  = -1; }
    if (ch.stdout_fd >= 0) { ::_close(ch.stdout_fd); ch.stdout_fd = -1; }
    if (ch.handle != nullptr) {
        HANDLE h = static_cast<HANDLE>(ch.handle);
        // ~100 ms of grace to exit on its own, then kill. Windows has no
        // SIGTERM, so TerminateProcess is the only escalation there is.
        if (::WaitForSingleObject(h, 100) != WAIT_OBJECT_0) {
            ::TerminateProcess(h, 1);
            ::WaitForSingleObject(h, 1000);
        }
        ::CloseHandle(h);
        ch.handle = nullptr;
    }
    ch.pid = -1;
#else
    if (ch.stdin_fd  >= 0) { ::close(ch.stdin_fd);  ch.stdin_fd  = -1; }
    if (ch.stdout_fd >= 0) { ::close(ch.stdout_fd); ch.stdout_fd = -1; }
    if (ch.pid > 0) {
        int st = 0;
        ::kill(static_cast<pid_t>(ch.pid), SIGTERM);
        ::waitpid(static_cast<pid_t>(ch.pid), &st, 0);
        ch.pid = -1;
    }
#endif
}

// ── Pipe / stdio byte moves ──────────────────────────────────────────────────
// These operate on CRT file descriptors (including 0 and 1), which both
// platforms provide. Windows spells the functions with an underscore.
[[nodiscard]] inline long fd_read(int fd, char* p, std::size_t n) noexcept {
#if defined(_WIN32)
    return ::_read(fd, p, static_cast<unsigned>(n));
#else
    for (;;) {
        const long r = static_cast<long>(::read(fd, p, n));
        if (r < 0 && errno == EINTR) continue;
        return r;
    }
#endif
}

[[nodiscard]] inline bool fd_write_all(int fd, std::string_view buf) noexcept {
    const char* p = buf.data();
    std::size_t n = buf.size();
    while (n > 0) {
#if defined(_WIN32)
        const long w = ::_write(fd, p, static_cast<unsigned>(n));
#else
        const long w = static_cast<long>(::write(fd, p, n));
        if (w < 0 && errno == EINTR) continue;
#endif
        if (w <= 0) return false;
        p += w;
        n -= static_cast<std::size_t>(w);
    }
    return true;
}

// Put stdin/stdout into binary mode. A no-op on POSIX; on Windows it is
// mandatory for the stdio transport, because the CRT defaults these to text
// mode and would otherwise inject '\r' into every newline-framed JSON message.
inline void set_stdio_binary() noexcept {
#if defined(_WIN32)
    ::_setmode(0, _O_BINARY);
    ::_setmode(1, _O_BINARY);
#endif
}

} // namespace rcp::plat
