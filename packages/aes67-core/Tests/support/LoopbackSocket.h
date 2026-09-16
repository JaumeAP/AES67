//
// LoopbackSocket.h
// AES67 core - test support
// The two ends of a loopback TCP conversation, for the suites that need a
// server that is a socket and a script rather than a real one.
//
// The listener setup was written five times across two packages -- in the
// driver's FakeRegistry, its SDP fetcher and RTSP suites, and in
// aes67-ravenna's HTTP client suite -- always the same seven calls: a socket,
// SO_REUSEADDR, a bind to port 0 on the loopback, getsockname to find out
// which port that turned out to be, and listen. What each of them then does
// with the accepted connection is genuinely different, and stays where it is.
//
// Nothing here binds a wildcard address: a test server that answers on every
// interface is a test server the machine's neighbours can reach.
//
#pragma once

#include <atomic>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdint>
#include <string>

namespace AES67 {
namespace TestSupport {

/// A listening socket on a port the kernel chose, on the loopback only.
class LoopbackListener {
public:
    LoopbackListener() = default;
    ~LoopbackListener() { close(); }

    // It owns a descriptor and closes it in the destructor, so a copy would
    // be two objects closing the same one -- and the second close lands on
    // whatever the process has since opened in its place. The five servers
    // this was lifted out of held a std::thread as well, which made them
    // uncopyable by accident; a plain value type has no such protection.
    LoopbackListener(const LoopbackListener&) = delete;
    LoopbackListener& operator=(const LoopbackListener&) = delete;
    LoopbackListener(LoopbackListener&&) = delete;
    LoopbackListener& operator=(LoopbackListener&&) = delete;

    /// Opens and listens. False leaves nothing open.
    bool open(int backlog = 4) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return false;

        int yes = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;  // whichever one is free
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            close();
            return false;
        }

        socklen_t len = sizeof(addr);
        if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            close();
            return false;
        }
        port_ = ntohs(addr.sin_port);

        if (::listen(fd_, backlog) != 0) {
            close();
            return false;
        }
        return true;
    }

    /// Wakes any thread blocked in accept() and closes the socket.
    ///
    /// The port goes with it: a listener that is closed has no port, and the
    /// three servers that hold one hand port() straight through to a test
    /// that is about to connect to it.
    void close() {
        const int fd = fd_.load(std::memory_order_relaxed);
        if (fd >= 0) {
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
            fd_.store(-1, std::memory_order_relaxed);
        }
        port_.store(0, std::memory_order_relaxed);
    }

    int fd() const { return fd_.load(std::memory_order_relaxed); }
    uint16_t port() const { return port_.load(std::memory_order_relaxed); }

private:
    // Atomic, not plain: the four servers this is shared by each run
    // accept() on a worker thread that reads fd() while the main thread's
    // destructor can be calling close() at the same time. ThreadSanitizer
    // caught the plain-int version of this as a real data race in
    // TestHTTPClient -- close()'s write and serve()'s read of fd_ racing,
    // undefined behavior whether or not the socket call itself was already
    // safe to make concurrently with a shutdown().
    std::atomic<int> fd_{-1};
    std::atomic<uint16_t> port_{0};
};

/// Sends one request to a server on the loopback and reads until the peer
/// closes, which is what these servers do after every answer.
///
/// Empty on any socket failure, so a test fails on the answer rather than
/// hanging: both timeouts are three seconds, and the reply is capped at 64 kB
/// so a server that never closes cannot grow this without bound either.
inline std::string askOnce(uint16_t port, const std::string& request) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return {};

    struct timeval tv{3, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return {};
    }

    if (::send(fd, request.data(), request.size(), 0) < 0) {
        ::close(fd);
        return {};
    }

    std::string response;
    char buffer[1024];
    while (true) {
        const ssize_t got = ::recv(fd, buffer, sizeof(buffer), 0);
        if (got <= 0) break;
        response.append(buffer, static_cast<size_t>(got));
        if (response.size() > 64 * 1024) break;
    }
    ::close(fd);
    return response;
}

} // namespace TestSupport
} // namespace AES67
