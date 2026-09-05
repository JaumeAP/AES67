//
// SDPFetcher.cpp
// AES67 macOS Driver
//

#include "NetworkEngine/Discovery/SDPFetcher.h"

#include "NetworkEngine/Discovery/HTTPClient.h"
#include "NetworkEngine/Discovery/RTSPClient.h"


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

/// A port out of a URL, without exceptions: std::stoi throws on garbage
/// and this runs inside coreaudiod.
bool parsePort(const std::string& text, uint16_t& out) {
    if (text.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long value = std::strtoul(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0' || errno == ERANGE) return false;
    if (value == 0 || value > 65535) return false;
    out = static_cast<uint16_t>(value);
    return true;
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
        if (!parsePort(hostPort.substr(colon + 1), out.port)) return false;
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
    // The GET, the bounded read and the defensive parsing all live in
    // HTTPClient now: the NMOS registration client needs the same thing
    // with a different verb, and two copies of a hand-rolled HTTP client
    // in one driver is one too many.
    HTTPClient client(parts.host, parts.port, timeoutMs);
    const HTTPResponse response = client.get(parts.path, "application/sdp");

    if (!response.error.empty()) {
        return {{}, response.error};
    }
    if (!response.ok()) {
        return {{}, "HTTP " + std::to_string(response.status) + " from " +
                        parts.host + parts.path};
    }
    if (response.body.empty()) {
        return {{}, "empty body from " + parts.host + parts.path};
    }
    if (response.body.size() > kMaxSDPBytes) {
        return {{}, "description larger than " + std::to_string(kMaxSDPBytes) + " bytes"};
    }
    return {response.body, {}};
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
