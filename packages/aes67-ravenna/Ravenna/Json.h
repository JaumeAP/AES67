//
// Json.h
// AES67 RAVENNA session layer
// Enough JSON for IS-05, and not one feature more.
//
// The connection API's payloads are small and their shape is fixed by the
// specification: objects of strings, numbers, booleans, nulls and a couple of
// nested objects. Pulling a library in for that would be a dependency this
// repository does not otherwise have, and writing a general JSON
// implementation would be writing far more than is used.
//
// So this parses what the API receives and writes what it sends. It does not
// parse floating point exponents, it does not preserve key order beyond
// sorting, and it says no rather than guessing: what comes in here arrives
// from the network.
//
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace AES67::Ravenna {

class JsonValue;
using JsonObject = std::map<std::string, JsonValue>;
using JsonArray = std::vector<JsonValue>;

/// One JSON value. Copyable, because the API hands whole staged objects
/// around and sharing them would mean deciding who owns what.
class JsonValue {
public:
    enum class Kind { Null, Bool, Number, String, Object, Array };

    JsonValue() = default;
    JsonValue(bool value) : kind_(Kind::Bool), boolean_(value) {}
    JsonValue(double value) : kind_(Kind::Number), number_(value) {}
    JsonValue(int value) : kind_(Kind::Number), number_(value) {}
    JsonValue(const char* value) : kind_(Kind::String), text_(value) {}
    JsonValue(std::string value) : kind_(Kind::String), text_(std::move(value)) {}
    JsonValue(JsonObject value) : kind_(Kind::Object), object_(std::move(value)) {}
    JsonValue(JsonArray value) : kind_(Kind::Array), array_(std::move(value)) {}

    Kind kind() const { return kind_; }
    bool isNull() const { return kind_ == Kind::Null; }
    bool isBool() const { return kind_ == Kind::Bool; }
    bool isNumber() const { return kind_ == Kind::Number; }
    bool isString() const { return kind_ == Kind::String; }
    bool isObject() const { return kind_ == Kind::Object; }
    bool isArray() const { return kind_ == Kind::Array; }

    bool asBool(bool fallback = false) const { return isBool() ? boolean_ : fallback; }
    double asNumber(double fallback = 0) const { return isNumber() ? number_ : fallback; }
    const std::string& asString() const { return text_; }
    const JsonObject& asObject() const { return object_; }
    const JsonArray& asArray() const { return array_; }
    JsonObject& asObject() { return object_; }

    /// The member, or a null value. Never throws: every read of a parsed
    /// payload goes through here, and a missing member is the normal case.
    const JsonValue& operator[](const std::string& key) const;

    bool has(const std::string& key) const;

    std::string serialise() const;

private:
    Kind kind_ = Kind::Null;
    bool boolean_ = false;
    double number_ = 0;
    std::string text_;
    JsonObject object_;
    JsonArray array_;
};

/// Parses `text`. False with `error` set on anything malformed, including
/// trailing rubbish after a complete value.
bool parseJson(const std::string& text, JsonValue& out, std::string& error);

/// A string as a JSON string literal, escapes and all.
std::string jsonQuote(const std::string& text);

}  // namespace AES67::Ravenna
