#include "NetworkEngine/JsonFields.h"

#include "NetworkEngine/JsonEscape.h"

#include <exception>
#include <limits>
#include <regex>

namespace AES67 {
namespace {

/// The value of `field`, as text, or nothing. `valuePattern` is what the value
/// looks like: every reader below differs only in that.
std::optional<std::string> matchField(const std::string& json, const std::string& field,
                                      const std::string& valuePattern) {
    // The key is put into a pattern, so a key with a regular expression
    // character in it would change what is being asked. Every caller names a
    // field of its own format, but that is the caller's discipline and not
    // this function's, so a key that is not a plain word is refused rather
    // than compiled.
    for (const char letter : field) {
        const bool plain = (letter >= 'a' && letter <= 'z') || (letter >= 'A' && letter <= 'Z') ||
                           (letter >= '0' && letter <= '9') || letter == '_' || letter == '-';
        if (!plain) return std::nullopt;
    }

    try {
        const std::regex pattern("\"" + field + "\"\\s*:\\s*" + valuePattern);
        std::smatch match;
        if (std::regex_search(json, match, pattern) && match.size() > 1) return match[1].str();
    } catch (const std::exception&) {
        // A document this cannot be run over is one with no such field in it.
        return std::nullopt;
    }
    return std::nullopt;
}

}  // namespace

std::optional<std::string> extractStringField(const std::string& json, const std::string& field) {
    // `(?:[^"\\]|\\.)*` rather than `[^"]*`: a value written by jsonEscape
    // contains `\"`, and stopping at the first quote returned the text up to
    // the backslash before it. The capture is still escaped, so it goes back
    // through jsonUnescape -- the two halves of the round trip are what this
    // was missing, not one of them.
    const auto text = matchField(json, field, "\"((?:[^\"\\\\]|\\\\.)*)\"");
    if (!text) return std::nullopt;
    return jsonUnescape(*text);
}

std::optional<uint64_t> extractUInt64Field(const std::string& json, const std::string& field) {
    const auto text = matchField(json, field, "(\\d+)");
    if (!text) return std::nullopt;
    try {
        return std::stoull(*text);
    } catch (const std::exception&) {
        // A number too large to hold is not a number this can report.
        return std::nullopt;
    }
}

namespace {

/// The uint64 value of `field`, if there is one and it fits in `Narrow`.
///
/// These used to static_cast the uint64 down, which is a silent reduction
/// modulo the width: a persisted port of 70000 came back as 4464 and the
/// driver bound a port nobody had asked for, and 65536 came back as 0 and was
/// then refused with "Invalid port: 0" -- an error naming a value that is not
/// in the file. The std::optional these return already says "no value I can
/// report", which is what an out-of-range number is.
template <typename Narrow>
std::optional<Narrow> narrowField(const std::string& json, const std::string& field) {
    const auto value = extractUInt64Field(json, field);
    if (!value) return std::nullopt;
    if (*value > static_cast<uint64_t>(std::numeric_limits<Narrow>::max())) return std::nullopt;
    return static_cast<Narrow>(*value);
}

}  // namespace

std::optional<uint32_t> extractUInt32Field(const std::string& json, const std::string& field) {
    return narrowField<uint32_t>(json, field);
}

std::optional<uint16_t> extractUInt16Field(const std::string& json, const std::string& field) {
    return narrowField<uint16_t>(json, field);
}

std::optional<uint8_t> extractUInt8Field(const std::string& json, const std::string& field) {
    return narrowField<uint8_t>(json, field);
}

std::optional<double> extractDoubleField(const std::string& json, const std::string& field) {
    const auto text = matchField(json, field, "([0-9.]+)");
    if (!text) return std::nullopt;
    try {
        return std::stod(*text);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<bool> extractBoolField(const std::string& json, const std::string& field) {
    const auto text = matchField(json, field, "(true|false)");
    if (!text) return std::nullopt;
    return *text == "true";
}

std::optional<int> extractIntField(const std::string& json, const std::string& field) {
    const auto text = matchField(json, field, "(-?\\d+)");
    if (!text) return std::nullopt;
    try {
        return std::stoi(*text);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

}  // namespace AES67
