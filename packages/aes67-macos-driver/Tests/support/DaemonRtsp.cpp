//
// DaemonRtsp.cpp
// AES67 macOS Driver - Tests
// See DaemonRtsp.h.
//
// Reproduced from the daemon's own source, read at
// https://github.com/bondagit/aes67-linux-daemon (daemon/rtsp_client.cpp,
// daemon/rtsp_client.hpp, daemon/rtsp_server.cpp), not vendored here.
//

#include "DaemonRtsp.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>
#include <vector>

namespace AES67 {
namespace Tests {

namespace {

std::string toLower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// boost::trim: strips leading and trailing whitespace, its default classifier.
std::string trim(const std::string& text) {
    const char* whitespace = " \t\r\n\f\v";
    const size_t start = text.find_first_not_of(whitespace);
    if (start == std::string::npos) return "";
    const size_t end = text.find_last_not_of(whitespace);
    return text.substr(start, end - start + 1);
}

bool startsWith(const std::string& text, const std::string& prefix) {
    return text.rfind(prefix, 0) == 0;
}

// boost::split(fields, line, boost::is_any_of(" ")): splits on every space,
// with no compression of consecutive delimiters -- two spaces in a row
// produce an empty field between them, same as the daemon sees it.
std::vector<std::string> splitOnSpace(const std::string& text) {
    std::vector<std::string> fields;
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == ' ') {
            fields.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    return fields;
}

// The daemon reads with std::getline(stream, line, '\n') and only then
// trims -- a line still carries its trailing '\r' at that point. Splitting
// requests here the same way keeps that carried-over '\r' in the same place
// the daemon has it when a check runs before the trim does.
std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line, '\n')) {
        lines.push_back(line);
    }
    return lines;
}

constexpr uint16_t kMaxBodyLength = 4096;  // RtspClient::max_body_length

} // namespace

std::string daemonEncodeUrl(const std::string& text) {
    std::string result;
    result.reserve(text.size());
    for (unsigned char c : text) {
        switch (c) {
            case ' ': result += "%20"; break;
            case '+': result += "%2B"; break;
            case '\r': result += "%0D"; break;
            case '\n': result += "%0A"; break;
            case '\'': result += "%27"; break;
            case ',': result += "%2C"; break;
            case ';': result += "%3B"; break;
            default:
                if (c >= 0x80) {
                    char hex[4];
                    const int written = std::snprintf(hex, sizeof(hex), "%02X", c);
                    (void)written;  // always 2 for a single byte
                    result += '%';
                    result += hex;
                } else {
                    result += static_cast<char>(c);
                }
        }
    }
    return result;
}

std::string daemonDescribeRequest(const std::string& host, uint16_t port,
                                  const std::string& path, int cseq) {
    std::ostringstream out;
    out << "DESCRIBE rtsp://" << host << ":" << port << daemonEncodeUrl(path)
        << " RTSP/1.0\r\n"
        << "CSeq: " << cseq << "\r\n"
        << "User-Agent: aes67-daemon\r\n"
        << "Accept: application/sdp\r\n\r\n";
    return out.str();
}

DaemonRtspDescribeResult daemonReadDescribeResponse(const std::string& response, int cseqSent) {
    DaemonRtspDescribeResult result;

    std::istringstream in(response);
    std::string version;
    unsigned int statusCode = 0;
    in >> version;
    in >> statusCode;
    std::string restOfStatusLine;
    std::getline(in, restOfStatusLine);

    if (in.fail() || version.substr(0, 5) != "RTSP/") {
        result.refusal = "invalid response";
        return result;
    }
    if (statusCode != 200) {
        result.refusal = "response with status code " + std::to_string(statusCode);
        return result;
    }

    int responseCseq = -1;
    std::string contentType;
    long long contentLength = 0;
    std::string header;
    while (std::getline(in, header) && header != "" && header != "\r") {
        header = trim(toLower(header));
        if (startsWith(header, "cseq:")) {
            try {
                responseCseq = std::stoi(header.substr(5));
            } catch (...) {
                // The daemon's own catch is around the whole header loop and
                // logs rather than failing the request; a malformed CSeq is
                // simply left at -1 here, same effect.
                responseCseq = -1;
            }
        } else if (startsWith(header, "content-type:")) {
            contentType = trim(header.substr(13));
        } else if (startsWith(header, "content-length:")) {
            try {
                contentLength = std::stoll(header.substr(15));
            } catch (...) {
                contentLength = 0;
            }
        }
    }

    if (responseCseq != cseqSent) {
        result.refusal = "invalid response sequence " + std::to_string(responseCseq);
        return result;
    }

    // Only a NON-empty, non-"application/sdp" Content-Type refuses the
    // response -- a response naming no content type at all is accepted as
    // if it were SDP. Reproduced exactly because it is easy to get backwards.
    if (!contentType.empty() && !startsWith(contentType, "application/sdp")) {
        result.refusal = "unsupported content-type " + contentType;
        return result;
    }

    if (contentLength > 0 && contentLength < kMaxBodyLength) {
        std::string body(static_cast<size_t>(contentLength), '\0');
        in.read(&body[0], contentLength);
        body.resize(static_cast<size_t>(in.gcount()));
        result.sdp = std::move(body);
    }

    result.accepted = true;
    return result;
}

std::string daemonDescribeOkResponse(int cseq, const std::string& sdp) {
    std::ostringstream out;
    out << "RTSP/1.0 200 OK\r\n"
        << "CSeq: " << cseq << "\r\n"
        << "Content-Length: " << sdp.length() << "\r\n"
        << "Content-Type: application/sdp\r\n"
        << "\r\n"
        << sdp;
    return out.str();
}

std::string daemonErrorResponse(int statusCode, const std::string& reason, int cseq) {
    std::ostringstream out;
    out << "RTSP/1.0 " << statusCode << " " << reason << "\r\n";
    if (cseq >= 0) {
        out << "CSeq: " << cseq << "\r\n";
    }
    out << "\r\n";
    return out.str();
}

DaemonRtspServerDecision daemonDecideRequest(const std::string& request, bool pathExists) {
    DaemonRtspServerDecision decision;

    const auto lines = splitLines(request);
    if (lines.empty()) {
        return decision;  // no request line at all: needs more data
    }

    std::string requestLine = trim(lines.front());
    const std::vector<std::string> fields = splitOnSpace(requestLine);
    if (fields.size() < 3) {
        return decision;  // malformed request line: needs more data, no reply
    }

    // Header lines, up to the blank one that ends them. A CSeq that fails to
    // parse breaks out of this loop WITHOUT the blank line ever being seen --
    // is_end stays false in the daemon's own code, which the caller reads as
    // "not a complete request yet" rather than as an error. No response is
    // sent for it, same as an incomplete one.
    int cseq = -1;
    bool sawBlankLine = false;
    for (size_t i = 1; i < lines.size(); ++i) {
        const std::string& raw = lines[i];
        if (raw.empty() || raw == "\r") {
            sawBlankLine = true;
            break;
        }
        const std::string header = trim(toLower(raw));
        if (startsWith(header, "cseq:")) {
            try {
                cseq = std::stoi(header.substr(5));
            } catch (...) {
                return decision;  // malformed CSeq: the daemon stops reading, no reply
            }
        }
    }
    if (!sawBlankLine) {
        return decision;  // headers not terminated yet: needs more data
    }

    if (startsWith(fields[0], "RTSP/")) {
        // A response line arriving where a request was expected: the daemon
        // reads past it silently and waits for the next request. This
        // driver's RTSPClient never sends anything shaped like this to a
        // server, so it is reproduced for completeness rather than exercised.
        decision.statusCode = -1;
        decision.reason = "response, not a request";
        return decision;
    }
    if (cseq < 0) {
        decision.statusCode = 400;
        decision.reason = "Bad Request";
        decision.cseq = cseq;
        return decision;
    }
    if (fields[2].substr(0, 5) != "RTSP/") {
        decision.statusCode = 400;
        decision.reason = "Bad Request";
        decision.cseq = cseq;
        return decision;
    }
    if (fields[0] != "DESCRIBE") {
        decision.statusCode = 405;
        decision.reason = "Method Not Allowed";
        decision.cseq = cseq;
        return decision;
    }

    decision.cseq = cseq;
    decision.path = trim(fields[1]);
    if (pathExists) {
        decision.statusCode = 200;
        decision.reason = "OK";
    } else {
        decision.statusCode = 404;
        decision.reason = "Not found";
    }
    return decision;
}

} // namespace Tests
} // namespace AES67
