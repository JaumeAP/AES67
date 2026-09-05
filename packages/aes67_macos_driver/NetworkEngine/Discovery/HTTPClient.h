//
// HTTPClient.h
// AES67 macOS Driver
//
// The small HTTP/1.1 client the discovery layer needs: fetching a session
// description from a URL, and registering with an NMOS registry.
//
// Plain HTTP only. This layer speaks BSD sockets and has no TLS, and a
// scheme that fails at connect time is worse than one that says so up
// front, so https is refused by the callers rather than half-supported
// here.
//
// Everything it reads comes from a server nobody here controls, into
// coreaudiod, where an escaped exception takes the audio daemon with it:
// no parse throws, the body is bounded, and both directions carry a
// timeout.
//
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace AES67 {

struct HTTPResponse {
    int status{0};        ///< 0 when nothing came back; the error says why.
    std::string body;
    std::string error;    ///< Empty exactly when the exchange completed.

    bool ok() const { return error.empty() && status >= 200 && status <= 299; }
};

class HTTPClient {
public:
    /// The largest body this will read. Everything it fetches — a session
    /// description, a registry's answer — is bytes.
    static constexpr size_t kMaxBodyBytes = 1 << 20; // 1 MiB

    static constexpr int kDefaultTimeoutMs = 3000;

    HTTPClient(std::string host, uint16_t port, int timeoutMs = kDefaultTimeoutMs);

    /// One request, one connection, closed at the end: registries and SDP
    /// servers are talked to rarely, and a kept-alive socket in a driver
    /// is a thread and a reconnect policy nobody asked for.
    HTTPResponse perform(const std::string& method,
                         const std::string& path,
                         const std::string& body = {},
                         const std::string& contentType = {});

    HTTPResponse get(const std::string& path, const std::string& accept = {});
    HTTPResponse post(const std::string& path, const std::string& body,
                      const std::string& contentType);
    HTTPResponse del(const std::string& path);

    const std::string& host() const { return host_; }
    uint16_t port() const { return port_; }

private:
    std::string host_;
    uint16_t port_;
    int timeoutMs_;
    std::string accept_;
};

} // namespace AES67
