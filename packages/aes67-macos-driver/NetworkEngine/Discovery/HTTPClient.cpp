//
// HTTPClient.cpp
// AES67 macOS Driver
//

#include "NetworkEngine/Discovery/HTTPClient.h"

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace AES67 {

namespace {

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// A small unsigned number out of text, without exceptions: std::stoi
/// throws on garbage and this runs inside coreaudiod.
bool parseSmallNumber(const std::string& text, long& out) {
    if (text.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || errno == ERANGE || value < 0) return false;
    out = value;
    return true;
}

/// One header's value, matched case-insensitively, out of a response head.
/// Written line by line rather than by index arithmetic on a lowercased
/// copy: the offsets in the two strings are the same only until somebody
/// prepends something to one of them, which is exactly the bug this shape
/// avoids.
std::string headerValue(const std::string& head, const std::string& name) {
    const std::string wanted = toLower(name);
    size_t lineStart = head.find("\r\n");
    if (lineStart == std::string::npos) return {};
    lineStart += 2;

    while (lineStart < head.size()) {
        size_t lineEnd = head.find("\r\n", lineStart);
        if (lineEnd == std::string::npos) lineEnd = head.size();

        const std::string line = head.substr(lineStart, lineEnd - lineStart);
        const size_t colon = line.find(':');
        if (colon != std::string::npos && toLower(line.substr(0, colon)) == wanted) {
            std::string value = line.substr(colon + 1);
            const size_t first = value.find_first_not_of(" \t");
            if (first == std::string::npos) return {};
            const size_t last = value.find_last_not_of(" \t");
            return value.substr(first, last - first + 1);
        }

        if (lineEnd == head.size()) break;
        lineStart = lineEnd + 2;
    }
    return {};
}

int connectTo(const std::string& host, uint16_t port, int timeoutMs, std::string& error) {
    struct addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* result = nullptr;
    const std::string portText = std::to_string(port);
    if (::getaddrinfo(host.c_str(), portText.c_str(), &hints, &result) != 0 || result == nullptr) {
        error = "cannot resolve " + host;
        return -1;
    }

    const int fd = ::socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (fd < 0) {
        ::freeaddrinfo(result);
        error = "cannot create a socket";
        return -1;
    }

    struct timeval timeout {};
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    if (::connect(fd, result->ai_addr, result->ai_addrlen) != 0) {
        ::close(fd);
        ::freeaddrinfo(result);
        error = "cannot connect to " + host + ":" + portText;
        return -1;
    }
    ::freeaddrinfo(result);
    return fd;
}

} // namespace

HTTPClient::HTTPClient(std::string host, uint16_t port, int timeoutMs)
    : host_(std::move(host)), port_(port), timeoutMs_(timeoutMs) {}

HTTPResponse HTTPClient::perform(const std::string& method,
                                 const std::string& path,
                                 const std::string& body,
                                 const std::string& contentType) {
    HTTPResponse response;

    std::string error;
    const int fd = connectTo(host_, port_, timeoutMs_, error);
    if (fd < 0) {
        response.error = error;
        return response;
    }

    std::ostringstream request;
    request << method << " " << path << " HTTP/1.1\r\n"
            << "Host: " << host_ << ":" << port_ << "\r\n"
            << "User-Agent: AES67macOSDriver\r\n";
    if (!accept_.empty()) request << "Accept: " << accept_ << "\r\n";
    if (!contentType.empty()) request << "Content-Type: " << contentType << "\r\n";
    request << "Content-Length: " << body.size() << "\r\n"
            << "Connection: close\r\n\r\n"
            << body;
    const std::string requestText = request.str();

    if (::send(fd, requestText.data(), requestText.size(), 0) < 0) {
        ::close(fd);
        response.error = "cannot send the request to " + host_;
        return response;
    }

    // Read until the server closes, bounded: a server that keeps talking
    // is cut off rather than followed.
    std::string raw;
    char chunk[4096];
    while (raw.size() <= kMaxBodyBytes + 8192) {
        const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) break;
        raw.append(chunk, static_cast<size_t>(n));
    }
    ::close(fd);

    if (raw.empty()) {
        response.error = "no answer from " + host_;
        return response;
    }

    const size_t headEnd = raw.find("\r\n\r\n");
    if (headEnd == std::string::npos) {
        response.error = "malformed answer from " + host_;
        return response;
    }
    const std::string head = raw.substr(0, headEnd);
    response.body = raw.substr(headEnd + 4);

    const size_t firstSpace = head.find(' ');
    if (firstSpace == std::string::npos) {
        response.error = "malformed status line from " + host_;
        return response;
    }
    const size_t secondSpace = head.find(' ', firstSpace + 1);
    const std::string statusText = head.substr(
        firstSpace + 1,
        secondSpace == std::string::npos ? std::string::npos : secondSpace - firstSpace - 1);
    long status = 0;
    if (!parseSmallNumber(statusText, status) || status < 100 || status > 599) {
        response.error = "malformed status line from " + host_;
        return response;
    }
    response.status = static_cast<int>(status);

    const std::string lengthText = headerValue(head, "Content-Length");
    if (!lengthText.empty()) {
        long declared = 0;
        if (!parseSmallNumber(lengthText, declared) ||
            static_cast<size_t>(declared) > kMaxBodyBytes) {
            response.status = 0;
            response.error = "the answer declares a body this will not read: " + lengthText;
            return response;
        }
        if (response.body.size() > static_cast<size_t>(declared)) {
            response.body.resize(static_cast<size_t>(declared));
        }
    }

    if (response.body.size() > kMaxBodyBytes) {
        response.status = 0;
        response.error = "body larger than " + std::to_string(kMaxBodyBytes) + " bytes";
        return response;
    }

    return response;
}

HTTPResponse HTTPClient::get(const std::string& path, const std::string& accept) {
    accept_ = accept;
    HTTPResponse response = perform("GET", path);
    accept_.clear();
    return response;
}

HTTPResponse HTTPClient::post(const std::string& path, const std::string& body,
                              const std::string& contentType) {
    return perform("POST", path, body, contentType);
}

HTTPResponse HTTPClient::del(const std::string& path) {
    return perform("DELETE", path);
}

} // namespace AES67
