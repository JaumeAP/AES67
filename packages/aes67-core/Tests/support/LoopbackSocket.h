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
    ~LoopbackListener() { close(); }

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
    void close() {
        if (fd_ >= 0) {
            ::shutdown(fd_, SHUT_RDWR);
            ::close(fd_);
            fd_ = -1;
        }
    }

    int fd() const { return fd_; }
    uint16_t port() const { return port_; }

private:
    int fd_{-1};
    uint16_t port_{0};
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
