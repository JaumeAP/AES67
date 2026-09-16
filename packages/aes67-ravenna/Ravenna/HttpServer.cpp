#include "Ravenna/ListenSocket.h"
#include "Ravenna/HttpServer.h"
#include "Ravenna/ApiReplies.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace AES67::Ravenna {
namespace {

constexpr size_t kMaxRequestBytes = 65536;
constexpr int kRequestTimeoutMs = 300;
/// A ceiling on the whole connection, not just on one silent poll().
/// kRequestTimeoutMs bounds how long a peer may say NOTHING; a peer that sends
/// one byte just inside it never trips it, so 64 KiB of request was still
/// hours inside this loop -- and this is the daemon's only thread, so its
/// whole main loop waits with it.
constexpr int kRequestDeadlineMs = 10000;

std::string statusTextFor(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        default: return "Error";
    }
}

/// The value of one hexadecimal digit, or -1 for anything else.
int hexValue(char digit) {
    if (digit >= '0' && digit <= '9') return digit - '0';
    if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
    if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
    return -1;
}

/// Percent-decoding, as RFC 3986 sec 2.1 says a path is written: a client may
/// escape any character of a segment, and several do. A device that compares
/// the raw text answers 404 for a resource that is sitting right there.
///
/// An escaped slash is left escaped on purpose. The path is routed as one
/// string and split on slashes by whichever API answers it, so decoding %2F
/// would turn one segment into two and fetch a resource nobody asked for.
/// An escape that is not one -- a bare percent, a bad digit -- stays as it is
/// rather than eating what follows: these paths come off the network.
///
/// RtspMessages.h has a percentDecode that does resolve %2F, and the two are
/// deliberately not one function. An RTSP request line is decoded whole and
/// the result compared against a session name, where a slash is just a
/// character; an HTTP path is decoded and then re-split on its separators,
/// where a slash is the separator. Merging them would have to break one of
/// those two, so they stay apart and say why.
std::string decodePath(const std::string& path) {
    std::string decoded;
    decoded.reserve(path.size());

    for (size_t i = 0; i < path.size(); ++i) {
        const int high = (path[i] == '%' && i + 2 < path.size()) ? hexValue(path[i + 1]) : -1;
        const int low = high >= 0 ? hexValue(path[i + 2]) : -1;
        if (low < 0) {
            decoded.push_back(path[i]);
            continue;
        }

        const char character = static_cast<char>(high * 16 + low);
        if (character == '/') {
            decoded.append(path, i, 3);
        } else {
            decoded.push_back(character);
        }
        i += 2;
    }
    return decoded;
}


}  // namespace

bool parseHttpRequest(const std::string& text, std::string& method, std::string& path,
                      std::string& body) {
    method.clear();
    path.clear();
    body.clear();

    const auto blank = text.find("\r\n\r\n");
    const size_t headerEnd = blank != std::string::npos ? blank + 4 : text.find("\n\n") + 2;
    if (blank == std::string::npos && text.find("\n\n") == std::string::npos) return false;

    std::istringstream stream(text.substr(0, headerEnd));
    std::string line;
    if (!std::getline(stream, line)) return false;

    std::string version;
    std::istringstream requestLine(trimmed(line));
    if (!(requestLine >> method >> path >> version)) return false;
    if (version.rfind("HTTP/", 0) != 0) return false;

    const auto query = path.find('?');
    // resize, not substr: the result is assigned back to the same string, so
    // a copy is made and thrown away.
    if (query != std::string::npos) path.resize(query);
    // After the query is gone, so an escaped '?' cannot cut the path short.
    path = decodePath(path);

    body = text.substr(headerEnd);
    return true;
}

std::string buildHttpResponse(int status, const std::string& contentType,
                              const std::string& body) {
    std::ostringstream response;
    response << "HTTP/1.1 " << status << ' ' << statusTextFor(status) << "\r\n";
    response << "Content-Type: " << contentType << "\r\n";
    response << "Content-Length: " << body.size() << "\r\n";
    // IS-05 controllers are often browser based, and a device that answers
    // without these is a device they cannot read: a preflight that comes back
    // without Allow-Headers fails the request before it is sent, which is
    // what the AMWA test suite catches and what a browser does silently.
    response << "Access-Control-Allow-Origin: *\r\n";
    response << "Access-Control-Allow-Methods: GET, PUT, POST, PATCH, DELETE, HEAD, OPTIONS\r\n";
    response << "Access-Control-Allow-Headers: Content-Type, Accept, Authorization\r\n";
    response << "Access-Control-Max-Age: 3600\r\n";
    response << "Connection: close\r\n";
    response << "\r\n";
    response << body;
    return response.str();
}

HttpServer::~HttpServer() { stop(); }

void HttpServer::stop() {
    if (listener_ >= 0) ::close(listener_);
    listener_ = -1;
    port_ = 0;
}

bool HttpServer::start(uint16_t port, std::string& error) {
    if (!openListenSocket(port, listener_, error)) {
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

void HttpServer::answer(int client, const std::string& text) {
    std::string method;
    std::string path;
    std::string body;

    std::string response;
    if (!parseHttpRequest(text, method, path, body)) {
        response = buildHttpResponse(400, "text/plain", "not an HTTP request\n");
    } else if (method == "OPTIONS") {
        // The preflight a browser-based controller sends before a PATCH.
        response = buildHttpResponse(200, "text/plain", {});
    } else {
        const ApiResponse answer = handler_(method, path, body);
        response = buildHttpResponse(answer.status, answer.contentType, answer.body);
    }

    ::send(client, response.data(), response.size(), 0);
}

size_t HttpServer::service() {
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
        size_t expectedBody = std::string::npos;

        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(kRequestDeadlineMs);

        while (text.size() < kMaxRequestBytes) {
            if (std::chrono::steady_clock::now() >= deadline) break;
            struct pollfd reading {};
            reading.fd = client;
            reading.events = POLLIN;
            if (::poll(&reading, 1, kRequestTimeoutMs) <= 0) break;

            char buffer[2048];
            const ssize_t received = ::recv(client, buffer, sizeof(buffer), 0);
            if (received <= 0) break;
            text.append(buffer, static_cast<size_t>(received));

            const auto blank = text.find("\r\n\r\n");
            if (blank == std::string::npos) continue;

            if (expectedBody == std::string::npos) {
                // Content-Length decides when a PATCH is complete: without it
                // the loop would wait for the timeout on every request that
                // has a body.
                expectedBody = 0;
                const std::string headers = text.substr(0, blank);
                const auto at = headers.find("Content-Length:");
                const auto atLower = headers.find("content-length:");
                const auto found = at != std::string::npos ? at : atLower;
                if (found != std::string::npos) {
                    expectedBody = static_cast<size_t>(
                        std::strtoul(headers.c_str() + found + 15, nullptr, 10));
                }
            }
            if (text.size() >= blank + 4 + expectedBody) break;
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
