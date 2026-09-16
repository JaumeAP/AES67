#include "Ravenna/ListenSocket.h"
#include "Ravenna/RtspServer.h"

#include "Ravenna/RtspMessages.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
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
/// A ceiling on the whole connection, not just on one silent poll(): a peer
/// sending one byte just inside kRequestTimeoutMs never trips it, and this
/// loop serves one connection at a time.
constexpr int kRequestDeadlineMs = 5000;

}  // namespace

RtspServer::~RtspServer() { stop(); }

void RtspServer::stop() {
    if (listener_ >= 0) ::close(listener_);
    listener_ = -1;
    port_ = 0;
}

bool RtspServer::start(uint16_t port, std::string& error) {
    if (!openListenSocket(port, listener_, error,
                          " (554 needs privilege; any port above 1024 works,"
                          " the SRV record carries it)")) {
        // stop() rather than clearing the descriptor by hand: it is also what
        // puts port_ back to 0, and a server that failed to start must not
        // still answer port() with the one it had before -- that number goes
        // straight into an mDNS SRV record.
        stop();
        return false;
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
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(kRequestDeadlineMs);
        while (text.size() < kMaxRequestBytes) {
            if (std::chrono::steady_clock::now() >= deadline) break;
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
