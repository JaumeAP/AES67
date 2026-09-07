#include "Ravenna/RtspServer.h"

#include "Ravenna/RtspMessages.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace AES67::Ravenna {
namespace {

/// A request this server answers is a request line and a few headers. A
/// client that sends more than this is not asking either of the two questions
/// it can answer.
constexpr size_t kMaxRequestBytes = 8192;

/// How long to wait for the rest of a request once a connection is accepted.
/// Long enough for a device on the same link, short enough that a connection
/// that says nothing cannot hold the loop.
constexpr int kRequestTimeoutMs = 200;

}  // namespace

RtspServer::~RtspServer() { stop(); }

void RtspServer::stop() {
    if (listener_ >= 0) ::close(listener_);
    listener_ = -1;
    port_ = 0;
}

bool RtspServer::start(uint16_t port, std::string& error) {
    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener_ < 0) {
        error = std::string("socket(): ") + std::strerror(errno);
        return false;
    }

    int on = 1;
    ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    struct sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (::bind(listener_, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) < 0) {
        error = "bind " + std::to_string(port) + ": " + std::strerror(errno) +
                (errno == EACCES ? " (554 needs privilege; any port above 1024 works,"
                                   " the SRV record carries it)"
                                 : "");
        stop();
        return false;
    }

    if (::listen(listener_, 8) < 0) {
        error = std::string("listen(): ") + std::strerror(errno);
        stop();
        return false;
    }

    if (port == 0) {
        // Port zero means "any", and the caller has to be told which, or the
        // SRV record advertises a port nothing is listening on.
        socklen_t length = sizeof(address);
        if (::getsockname(listener_, reinterpret_cast<struct sockaddr*>(&address),
                          &length) == 0) {
            port = ntohs(address.sin_port);
        }
    }
    port_ = port;
    return true;
}

void RtspServer::answer(int client, const std::string& text) {
    RtspRequest request;
    if (!parseRtspRequest(text, request)) return;  // not RTSP: say nothing

    std::string response;
    switch (request.method) {
        case RtspMethod::Options:
            response = buildOptionsResponse(request.sequence);
            break;

        case RtspMethod::Describe: {
            // The URI is usually absolute -- rtsp://host:port/by-name/x --
            // and what identifies the session is the path.
            std::string path = request.uri;
            const auto scheme = path.find("://");
            if (scheme != std::string::npos) {
                const auto slash = path.find('/', scheme + 3);
                path = slash == std::string::npos ? "/" : path.substr(slash);
            }

            if (const auto sdp = catalogue_.describe(percentDecode(path))) {
                response = buildDescribeResponse(request.sequence, request.uri, *sdp);
            } else {
                response = buildNotFoundResponse(request.sequence);
            }
            break;
        }

        case RtspMethod::Unsupported:
            response = buildNotImplementedResponse(request.sequence);
            break;
    }

    ::send(client, response.data(), response.size(), 0);
}

size_t RtspServer::service() {
    if (listener_ < 0) return 0;

    size_t answered = 0;
    while (true) {
        struct pollfd waiting {};
        waiting.fd = listener_;
        waiting.events = POLLIN;
        if (::poll(&waiting, 1, 0) <= 0) break;

        const int client = ::accept(listener_, nullptr, nullptr);
        if (client < 0) break;

        std::string text;
        while (text.size() < kMaxRequestBytes) {
            struct pollfd reading {};
            reading.fd = client;
            reading.events = POLLIN;
            if (::poll(&reading, 1, kRequestTimeoutMs) <= 0) break;

            char buffer[1024];
            const ssize_t received = ::recv(client, buffer, sizeof(buffer), 0);
            if (received <= 0) break;
            text.append(buffer, static_cast<size_t>(received));
            if (hasCompleteHeaders(text)) break;
        }

        if (!text.empty()) {
            answer(client, text);
            ++answered;
        }
        ::close(client);
    }
    return answered;
}

}  // namespace AES67::Ravenna
