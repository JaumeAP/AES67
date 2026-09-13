#include "Ravenna/ApiReplies.h"

#include <arpa/inet.h>

#include <algorithm>
#include <cctype>

namespace AES67::Ravenna {

ApiResponse jsonResponse(int status, const JsonValue& value) {
    ApiResponse response;
    response.status = status;
    response.body = value.serialise();
    return response;
}

ApiResponse errorResponse(int status, const std::string& detail) {
    // The object NMOS defines for an error: the code, a summary and the
    // detail, and a controller shows the detail to a person.
    JsonObject error;
    error["code"] = JsonValue(status);
    error["error"] = JsonValue(status == 404   ? "Not Found"
                               : status == 405 ? "Method Not Allowed"
                               : status == 423 ? "Locked"
                               : status == 501 ? "Not Implemented"
                                               : "Bad Request");
    error["debug"] = JsonValue(detail);
    return jsonResponse(status, JsonValue(error));
}

ApiResponse listResponse(const std::vector<std::string>& entries) {
    JsonArray items;
    items.reserve(entries.size());
    for (const std::string& entry : entries) items.emplace_back(entry);
    return jsonResponse(200, JsonValue(items));
}

std::vector<std::string> segmentsOf(const std::string& path) {
    std::vector<std::string> segments;
    size_t start = 0;

    while (start <= path.size()) {
        const size_t slash = path.find('/', start);
        const std::string segment =
            path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (!segment.empty()) segments.push_back(segment);
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return segments;
}

std::string addressText(uint32_t addressV4) {
    struct in_addr address {};
    address.s_addr = htonl(addressV4);
    char text[INET_ADDRSTRLEN] = {};
    if (::inet_ntop(AF_INET, &address, text, sizeof(text)) == nullptr) return {};
    return text;
}

std::string lowered(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char letter) { return static_cast<char>(std::tolower(letter)); });
    return text;
}

std::string trimmed(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

}  // namespace AES67::Ravenna
