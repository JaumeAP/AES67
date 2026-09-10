#include "Ravenna/Json.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace AES67::Ravenna {
namespace {

const JsonValue kNull{};

struct Parser {
    const std::string& text;
    size_t at = 0;
    std::string error;

    explicit Parser(const std::string& source) : text(source) {}

    void skipSpace() {
        while (at < text.size() &&
               (text[at] == ' ' || text[at] == '\t' || text[at] == '\n' || text[at] == '\r')) {
            ++at;
        }
    }

    bool fail(const std::string& what) {
        if (error.empty()) {
            error = what + " at offset " + std::to_string(at);
        }
        return false;
    }

    bool literal(const char* word) {
        const size_t length = std::char_traits<char>::length(word);
        if (text.compare(at, length, word) != 0) return fail("expected " + std::string(word));
        at += length;
        return true;
    }

    bool parseString(std::string& out) {
        if (at >= text.size() || text[at] != '"') return fail("expected a string");
        ++at;
        out.clear();

        while (at < text.size()) {
            const char c = text[at++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (at >= text.size()) return fail("a string ends inside an escape");

            switch (text[at++]) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    // Only the Basic Latin range is decoded, which is all an
                    // IS-05 payload carries. Anything else is kept as it was
                    // written rather than mangled into a wrong character.
                    if (at + 4 > text.size()) return fail("a short \\u escape");
                    const std::string digits = text.substr(at, 4);
                    at += 4;
                    const long code = std::strtol(digits.c_str(), nullptr, 16);
                    if (code > 0 && code < 0x80) {
                        out += static_cast<char>(code);
                    } else {
                        out += "\\u" + digits;
                    }
                    break;
                }
                default: return fail("an escape this parser does not know");
            }
        }
        return fail("a string that never ends");
    }

    bool parseValue(JsonValue& out) {
        skipSpace();
        if (at >= text.size()) return fail("nothing to parse");

        const char c = text[at];
        if (c == '{') return parseObject(out);
        if (c == '[') return parseArray(out);
        if (c == '"') {
            std::string value;
            if (!parseString(value)) return false;
            out = JsonValue(value);
            return true;
        }
        if (c == 't') { if (!literal("true")) return false; out = JsonValue(true); return true; }
        if (c == 'f') { if (!literal("false")) return false; out = JsonValue(false); return true; }
        if (c == 'n') { if (!literal("null")) return false; out = JsonValue(); return true; }

        // A number. strtod takes what JSON allows and a little more; the
        // little more is not worth a hand-written scanner.
        const char* start = text.c_str() + at;
        char* stop = nullptr;
        const double value = std::strtod(start, &stop);
        if (stop == start) return fail("not a value");
        at += static_cast<size_t>(stop - start);
        out = JsonValue(value);
        return true;
    }

    bool parseObject(JsonValue& out) {
        ++at;  // '{'
        JsonObject members;
        skipSpace();

        if (at < text.size() && text[at] == '}') {
            ++at;
            out = JsonValue(members);
            return true;
        }

        while (true) {
            skipSpace();
            std::string key;
            if (!parseString(key)) return false;
            skipSpace();
            if (at >= text.size() || text[at] != ':') return fail("expected ':'");
            ++at;

            JsonValue value;
            if (!parseValue(value)) return false;
            members[key] = value;

            skipSpace();
            if (at >= text.size()) return fail("an object that never closes");
            if (text[at] == ',') { ++at; continue; }
            if (text[at] == '}') { ++at; break; }
            return fail("expected ',' or '}'");
        }

        out = JsonValue(members);
        return true;
    }

    bool parseArray(JsonValue& out) {
        ++at;  // '['
        JsonArray items;
        skipSpace();

        if (at < text.size() && text[at] == ']') {
            ++at;
            out = JsonValue(items);
            return true;
        }

        while (true) {
            JsonValue value;
            if (!parseValue(value)) return false;
            items.push_back(value);

            skipSpace();
            if (at >= text.size()) return fail("an array that never closes");
            if (text[at] == ',') { ++at; continue; }
            if (text[at] == ']') { ++at; break; }
            return fail("expected ',' or ']'");
        }

        out = JsonValue(items);
        return true;
    }
};

}  // namespace

const JsonValue& JsonValue::operator[](const std::string& key) const {
    if (!isObject()) return kNull;
    const auto found = object_.find(key);
    return found == object_.end() ? kNull : found->second;
}

bool JsonValue::has(const std::string& key) const {
    return isObject() && object_.find(key) != object_.end();
}

std::string jsonQuote(const std::string& text) {
    std::string quoted = "\"";
    for (const char c : text) {
        switch (c) {
            case '"': quoted += "\\\""; break;
            case '\\': quoted += "\\\\"; break;
            case '\b': quoted += "\\b"; break;
            case '\f': quoted += "\\f"; break;
            case '\n': quoted += "\\n"; break;
            case '\r': quoted += "\\r"; break;
            case '\t': quoted += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char escape[7];
                    (void)std::snprintf(escape, sizeof(escape), "\\u%04x", c); // 6 chars into 7, always
                    quoted += escape;
                } else {
                    quoted += c;
                }
        }
    }
    quoted += '"';
    return quoted;
}

std::string JsonValue::serialise() const {
    switch (kind_) {
        case Kind::Null: return "null";
        case Kind::Bool: return boolean_ ? "true" : "false";
        case Kind::String: return jsonQuote(text_);

        case Kind::Number: {
            // Whole numbers as integers: IS-05 carries ports, counts and
            // times, and "5004.000000" is a port number nobody wants to read.
            if (number_ == std::floor(number_) && std::abs(number_) < 1e15) {
                return std::to_string(static_cast<long long>(number_));
            }
            std::ostringstream out;
            out.precision(10);
            out << number_;
            return out.str();
        }

        case Kind::Object: {
            std::string out = "{";
            bool first = true;
            for (const auto& [key, value] : object_) {
                if (!first) out += ',';
                first = false;
                out += jsonQuote(key) + ':' + value.serialise();
            }
            return out + '}';
        }

        case Kind::Array: {
            std::string out = "[";
            for (size_t i = 0; i < array_.size(); ++i) {
                if (i > 0) out += ',';
                out += array_[i].serialise();
            }
            return out + ']';
        }
    }
    return "null";
}

bool parseJson(const std::string& text, JsonValue& out, std::string& error) {
    Parser parser(text);
    if (!parser.parseValue(out)) {
        error = parser.error;
        return false;
    }

    parser.skipSpace();
    if (parser.at != text.size()) {
        error = "trailing text after the value, at offset " + std::to_string(parser.at);
        return false;
    }
    error.clear();
    return true;
}

}  // namespace AES67::Ravenna
