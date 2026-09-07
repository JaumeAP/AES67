#include "Ravenna/RtspMessages.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace AES67::Ravenna {
namespace {

std::string lowered(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string trimmed(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

RtspMethod methodFrom(const std::string& name) {
    if (name == "OPTIONS") return RtspMethod::Options;
    if (name == "DESCRIBE") return RtspMethod::Describe;
    return RtspMethod::Unsupported;
}

}  // namespace

std::string percentDecode(const std::string& text) {
    std::string decoded;
    decoded.reserve(text.size());

    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '%' || i + 2 >= text.size()) {
            decoded += text[i];
            continue;
        }
        const char high = text[i + 1];
        const char low = text[i + 2];
        if (std::isxdigit(static_cast<unsigned char>(high)) == 0 ||
            std::isxdigit(static_cast<unsigned char>(low)) == 0) {
            decoded += text[i];
            continue;
        }
        decoded += static_cast<char>(std::stoi(std::string{high, low}, nullptr, 16));
        i += 2;
    }
    return decoded;
}

bool hasCompleteHeaders(const std::string& text) {
    return text.find("\r\n\r\n") != std::string::npos ||
           text.find("\n\n") != std::string::npos;
}

bool parseRtspRequest(const std::string& text, RtspRequest& out) {
    out = RtspRequest{};

    std::istringstream stream(text);
    std::string line;
    if (!std::getline(stream, line)) return false;

    std::istringstream requestLine(trimmed(line));
    if (!(requestLine >> out.methodName >> out.uri >> out.version)) return false;
    // Anything else is not RTSP, whatever else it may be. Answering it would
    // mean answering a stray HTTP scan as though it had asked for a session.
    if (out.version.rfind("RTSP/", 0) != 0) return false;

    out.method = methodFrom(out.methodName);

    while (std::getline(stream, line)) {
        const std::string header = trimmed(line);
        if (header.empty()) break;  // end of the header block

        const auto colon = header.find(':');
        if (colon == std::string::npos) continue;
        const std::string name = lowered(trimmed(header.substr(0, colon)));
        const std::string value = trimmed(header.substr(colon + 1));
        out.headers[name] = value;
    }

    const auto sequence = out.headers.find("cseq");
    if (sequence != out.headers.end()) {
        try {
            out.sequence = static_cast<uint32_t>(std::stoul(sequence->second));
        } catch (...) {
            out.sequence = 0;  // present and unreadable is the same as absent
        }
    }
    return true;
}

std::string buildRtspResponse(uint32_t sequence, int statusCode,
                              const std::string& statusText,
                              const std::map<std::string, std::string>& headers,
                              const std::string& body) {
    std::ostringstream response;
    response << "RTSP/1.0 " << statusCode << ' ' << statusText << "\r\n";
    response << "CSeq: " << sequence << "\r\n";
    for (const auto& [name, value] : headers) {
        response << name << ": " << value << "\r\n";
    }
    if (!body.empty()) {
        response << "Content-Length: " << body.size() << "\r\n";
    }
    response << "\r\n";
    response << body;
    return response.str();
}

std::string buildDescribeResponse(uint32_t sequence, const std::string& contentBase,
                                  const std::string& sdp) {
    return buildRtspResponse(sequence, 200, "OK",
                             {{"Content-Type", "application/sdp"},
                              {"Content-Base", contentBase}},
                             sdp);
}

std::string buildOptionsResponse(uint32_t sequence) {
    return buildRtspResponse(sequence, 200, "OK",
                             {{"Public", "OPTIONS, DESCRIBE"}}, {});
}

std::string buildNotImplementedResponse(uint32_t sequence) {
    return buildRtspResponse(sequence, 501, "Not Implemented", {}, {});
}

std::string buildNotFoundResponse(uint32_t sequence) {
    return buildRtspResponse(sequence, 404, "Not Found", {}, {});
}

}  // namespace AES67::Ravenna
