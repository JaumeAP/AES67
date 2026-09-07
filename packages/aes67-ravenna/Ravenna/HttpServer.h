//
// HttpServer.h
// AES67 RAVENNA session layer
// The socket half of IS-05.
//
// Enough HTTP/1.1 to serve a connection API to a controller: GET and PATCH,
// a Content-Length body, one request per connection. No keep-alive, no
// chunked encoding, no TLS -- IS-05 over HTTPS is a deployment decision and
// belongs behind a proxy, not inside a daemon this size.
//
// The same shape as RtspServer: poll, accept, read, answer, close. Nothing
// here decides what a request means; ConnectionApi does.
//
#pragma once

#include "Ravenna/ConnectionApi.h"

#include <cstdint>
#include <string>

namespace AES67::Ravenna {

class HttpServer {
public:
    explicit HttpServer(ConnectionApi& api) : api_(api) {}
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    bool start(uint16_t port, std::string& error);
    void stop();

    /// Answers what is waiting. Returns how many requests were answered.
    size_t service();

    uint16_t port() const { return port_; }

private:
    void answer(int client, const std::string& request);

    ConnectionApi& api_;
    int listener_ = -1;
    uint16_t port_ = 0;
};

/// Splits a request into its method, its path and its body. False when the
/// text is not an HTTP request. The query string is dropped: nothing in this
/// API reads one, and leaving it on the path would fail every match.
bool parseHttpRequest(const std::string& text, std::string& method, std::string& path,
                      std::string& body);

/// The status line, the headers and the body. `contentType` is written even
/// for an empty body, because a controller reads it before it reads the body.
std::string buildHttpResponse(int status, const std::string& contentType,
                              const std::string& body);

}  // namespace AES67::Ravenna
