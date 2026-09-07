#include "Ravenna/HttpServer.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace AES67::Ravenna {
namespace {

constexpr size_t kMaxRequestBytes = 65536;
constexpr int kRequestTimeoutMs = 300;

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

std::string trimmed(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
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
    if (query != std::string::npos) path = path.substr(0, query);

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
    // without this is a device they cannot read.
    response << "Access-Control-Allow-Origin: *\r\n";
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
        error = "bind " + std::to_string(port) + ": " + std::strerror(errno);
        stop();
        return false;
    }
    if (::listen(listener_, 8) < 0) {
        error = std::string("listen(): ") + std::strerror(errno);
        stop();
        return false;
    }

    if (port == 0) {
        socklen_t length = sizeof(address);
        if (::getsockname(listener_, reinterpret_cast<struct sockaddr*>(&address), &length) == 0) {
            port = ntohs(address.sin_port);
        }
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

        while (text.size() < kMaxRequestBytes) {
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
