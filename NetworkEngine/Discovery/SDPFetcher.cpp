//
// SDPFetcher.cpp
// AES67 macOS Driver
//

#include "NetworkEngine/Discovery/SDPFetcher.h"

#include "NetworkEngine/Discovery/RTSPClient.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace AES67 {

namespace {

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// A small unsigned number out of text, without exceptions: std::stoi
/// throws on garbage and this runs inside coreaudiod. Used for the port in
/// a URL and for the status code in an answer, both of which are bounded.
bool parseSmallNumber(const std::string& text, uint16_t& out) {
    if (text.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long value = std::strtoul(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0' || errno == ERANGE) return false;
    if (value == 0 || value > 65535) return false;
    out = static_cast<uint16_t>(value);
    return true;
}

/// A header value, case-insensitively, out of an HTTP response head.
std::string headerValue(const std::string& head, const std::string& name) {
    const std::string lowerHead = toLower(head);
    const std::string lowerName = toLower(name) + ":";
    size_t pos = lowerHead.find("\r\n" + lowerName);
    if (pos == std::string::npos) return {};
    pos += 2 + lowerName.size();
    const size_t end = head.find("\r\n", pos);
    std::string value = head.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    const size_t first = value.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    const size_t last = value.find_last_not_of(" \t");
    return value.substr(first, last - first + 1);
}

/// Connects with a timeout on both directions, or returns -1.
int connectTo(const std::string& host, uint16_t port, int timeoutMs, std::string& error) {
    struct addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* result = nullptr;
    const std::string portText = std::to_string(port);
    const int rc = ::getaddrinfo(host.c_str(), portText.c_str(), &hints, &result);
    if (rc != 0 || result == nullptr) {
        error = "cannot resolve " + host;
        return -1;
    }

    int fd = ::socket(result->ai_family, result->ai_socktype, result->ai_protocol);
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

bool SDPFetcher::parseURL(const std::string& url, URLParts& out) {
    out = URLParts{};
    if (url.empty()) return false;

    const size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) {
        // A bare path. Not a failure: it is the form every settings file
        // used before this existed.
        out.path = url;
        return true;
    }

    out.scheme = toLower(url.substr(0, schemeEnd));
    const size_t hostStart = schemeEnd + 3;

    if (out.scheme == "file") {
        // file:///path -- the host part is empty and the path is what is left.
        out.path = url.substr(hostStart);
        if (!out.path.empty() && out.path.front() != '/') {
            // file://relative is not a thing; treat what follows as the path.
            out.path = "/" + out.path;
        }
        return !out.path.empty();
    }

    if (out.scheme != "http" && out.scheme != "rtsp" && out.scheme != "https") {
        return false;
    }

    const size_t pathStart = url.find('/', hostStart);
    std::string hostPort;
    if (pathStart == std::string::npos) {
        hostPort = url.substr(hostStart);
        out.path = "/";
    } else {
        hostPort = url.substr(hostStart, pathStart - hostStart);
        out.path = url.substr(pathStart);
    }
    if (hostPort.empty()) return false;

    const size_t colon = hostPort.find(':');
    if (colon == std::string::npos) {
        out.host = hostPort;
        out.port = (out.scheme == "rtsp") ? 554 : (out.scheme == "https" ? 443 : 80);
    } else {
        out.host = hostPort.substr(0, colon);
        if (out.host.empty()) return false;
        if (!parseSmallNumber(hostPort.substr(colon + 1), out.port)) return false;
    }
    return true;
}

SDPFetchResult SDPFetcher::fetch(const std::string& url, int timeoutMs) {
    URLParts parts;
    if (!parseURL(url, parts)) {
        return {{}, "not a URL this can read: " + url};
    }

    if (parts.scheme.empty() || parts.scheme == "file") {
        return fetchFile(parts.path);
    }
    if (parts.scheme == "http") {
        return fetchHTTP(parts, timeoutMs);
    }
    if (parts.scheme == "rtsp") {
        return fetchRTSP(url, parts, timeoutMs);
    }
    if (parts.scheme == "https") {
        return {{}, "https is not supported: this layer speaks plain sockets and has no TLS"};
    }
    return {{}, "unsupported scheme: " + parts.scheme};
}

SDPFetchResult SDPFetcher::fetchFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return {{}, "cannot open " + path};
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    std::string text = buffer.str();
    if (text.empty()) {
        return {{}, "empty file: " + path};
    }
    if (text.size() > kMaxSDPBytes) {
        return {{}, "description larger than " + std::to_string(kMaxSDPBytes) + " bytes"};
    }
    return {std::move(text), {}};
}

SDPFetchResult SDPFetcher::fetchHTTP(const URLParts& parts, int timeoutMs) {
    std::string error;
    const int fd = connectTo(parts.host, parts.port, timeoutMs, error);
    if (fd < 0) return {{}, error};

    std::ostringstream request;
    request << "GET " << parts.path << " HTTP/1.1\r\n"
            << "Host: " << parts.host << ":" << parts.port << "\r\n"
            << "Accept: application/sdp\r\n"
            << "User-Agent: AES67macOSDriver\r\n"
            << "Connection: close\r\n\r\n";
    const std::string requestText = request.str();
    if (::send(fd, requestText.data(), requestText.size(), 0) < 0) {
        ::close(fd);
        return {{}, "cannot send the request to " + parts.host};
    }

    // Read until the server closes, bounded: the head plus at most one
    // description. A server that keeps talking is cut off, not followed.
    std::string response;
    char chunk[4096];
    while (response.size() <= kMaxSDPBytes + 8192) {
        const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) break;
        response.append(chunk, static_cast<size_t>(n));
    }
    ::close(fd);

    if (response.empty()) {
        return {{}, "no answer from " + parts.host};
    }

    const size_t headEnd = response.find("\r\n\r\n");
    if (headEnd == std::string::npos) {
        return {{}, "malformed HTTP answer from " + parts.host};
    }
    const std::string head = response.substr(0, headEnd);
    std::string body = response.substr(headEnd + 4);

    // Status line: HTTP/1.1 200 OK
    const size_t firstSpace = head.find(' ');
    if (firstSpace == std::string::npos) {
        return {{}, "malformed status line from " + parts.host};
    }
    uint16_t status = 0;
    const size_t secondSpace = head.find(' ', firstSpace + 1);
    const std::string statusText = head.substr(
        firstSpace + 1,
        secondSpace == std::string::npos ? std::string::npos : secondSpace - firstSpace - 1);
    if (!parseSmallNumber(statusText, status)) {
        return {{}, "malformed status line from " + parts.host};
    }
    if (status < 200 || status > 299) {
        return {{}, "HTTP " + std::to_string(status) + " from " + parts.host + parts.path};
    }

    const std::string lengthText = headerValue("\r\n" + head, "Content-Length");
    if (!lengthText.empty()) {
        errno = 0;
        char* end = nullptr;
        const unsigned long declared = std::strtoul(lengthText.c_str(), &end, 10);
        if (end == lengthText.c_str() || errno == ERANGE || declared > kMaxSDPBytes) {
            return {{}, "the answer declares a body this will not read: " + lengthText};
        }
        if (body.size() > declared) {
            body.resize(static_cast<size_t>(declared));
        }
    }

    if (body.empty()) {
        return {{}, "empty body from " + parts.host + parts.path};
    }
    if (body.size() > kMaxSDPBytes) {
        return {{}, "description larger than " + std::to_string(kMaxSDPBytes) + " bytes"};
    }
    return {std::move(body), {}};
}

SDPFetchResult SDPFetcher::fetchRTSP(const std::string& url, const URLParts& parts,
                                     int timeoutMs) {
    RTSPClient client(url);
    client.setTimeout(timeoutMs);
    const auto session = client.describe(parts.path);
    const RTSPResponse& response = client.getLastResponse();
    if (!session) {
        if (response.statusCode == 0) {
            return {{}, "no answer to DESCRIBE from " + parts.host};
        }
        if (!response.isSuccess()) {
            return {{}, "RTSP " + std::to_string(response.statusCode) + " " +
                        response.statusMessage + " from " + parts.host + parts.path};
        }
        return {{}, "the DESCRIBE answer from " + parts.host + " is not a session description"};
    }
    if (response.body.empty()) {
        return {{}, "empty DESCRIBE body from " + parts.host + parts.path};
    }
    if (response.body.size() > kMaxSDPBytes) {
        return {{}, "description larger than " + std::to_string(kMaxSDPBytes) + " bytes"};
    }
    return {response.body, {}};
}

} // namespace AES67
