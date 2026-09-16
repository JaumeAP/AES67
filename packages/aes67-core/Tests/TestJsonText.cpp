//
// TestJsonText.cpp
// AES67 core
// Writing a string into JSON, and reading one field back out.
//
// Both halves had been written more than once -- the escaper twice, the field
// readers four times -- and the copies did not agree. This is what the one
// that survived has to do.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/JsonEscape.h"
#include "NetworkEngine/JsonFields.h"

namespace AES67 {
namespace Tests {

TEST_CASE("A control character does not escape the string it is in") {
    // The copy this one replaced escaped the quote and the backslash and let
    // everything else through, so a device name with a newline in it wrote a
    // settings file this repository could no longer read back.
    CHECK(jsonEscape("plain") == "plain");
    CHECK(jsonEscape("a \"quoted\" word") == "a \\\"quoted\\\" word");
    CHECK(jsonEscape("back\\slash") == "back\\\\slash");
    CHECK(jsonEscape("two\nlines") == "two\\nlines");
    CHECK(jsonEscape("carriage\rreturn") == "carriage\\rreturn");
    CHECK(jsonEscape("a\ttab") == "a\\ttab");

    // The ones with no name of their own go out as \u00XX, which is the only
    // general way RFC 8259 gives (sec 7).
    CHECK(jsonEscape(std::string("bell\x07")) == "bell\\u0007");
    CHECK(jsonEscape(std::string(1, '\x1f')) == "\\u001f");

    // And a space is not a control character.
    CHECK(jsonEscape(" ") == " ");
}

TEST_CASE("A field is read back with the type it was asked for") {
    const std::string json =
        R"({"name": "Mix A", "channels": 8, "gain": 1.5, "muted": true, )"
        R"("offset": -3, "big": 4294967296})";

    CHECK(*extractStringField(json, "name") == "Mix A");
    CHECK(*extractUInt16Field(json, "channels") == 8);
    CHECK(*extractDoubleField(json, "gain") == doctest::Approx(1.5));
    CHECK(*extractBoolField(json, "muted") == true);
    CHECK(*extractIntField(json, "offset") == -3);
    CHECK(*extractUInt64Field(json, "big") == 4294967296ULL);

    // A field that is not there is nothing, not a zero that reads like one.
    CHECK_FALSE(extractStringField(json, "label").has_value());
    CHECK_FALSE(extractUInt16Field(json, "label").has_value());
    CHECK_FALSE(extractBoolField(json, "label").has_value());
}

TEST_CASE("A key that is not a plain word is not compiled into a pattern") {
    // The key goes into a regular expression, so a key carrying one of its
    // characters would change the question being asked rather than fail to
    // match. Refused instead.
    const std::string json = R"({"name": "Mix A"})";
    CHECK_FALSE(extractStringField(json, "na.e").has_value());
    CHECK_FALSE(extractStringField(json, "(name)").has_value());
    CHECK_FALSE(extractStringField(json, "name|other").has_value());
    CHECK(extractStringField(json, "name").has_value());
}

TEST_CASE("A number too large for its type is not wrapped into another one") {
    CHECK_FALSE(extractUInt64Field(R"({"n": 99999999999999999999999})", "n").has_value());
    CHECK_FALSE(extractIntField(R"({"n": -99999999999999999999999})", "n").has_value());

    // This case had the title above and covered only the two readers that
    // honoured it. The narrower unsigned ones static_cast the uint64 down, so
    // a persisted port of 70000 came back as 4464 and the driver bound a port
    // nobody asked for, and 65536 came back as 0 and was then refused with
    // "Invalid port: 0" -- an error naming a value that is not in the file.
    CHECK_FALSE(extractUInt16Field(R"({"n": 70000})", "n").has_value());
    CHECK_FALSE(extractUInt16Field(R"({"n": 65536})", "n").has_value());
    CHECK_FALSE(extractUInt8Field(R"({"n": 300})", "n").has_value());
    CHECK_FALSE(extractUInt32Field(R"({"n": 4294967296})", "n").has_value());

    // The largest value each one can hold is still a value.
    CHECK(extractUInt16Field(R"({"n": 65535})", "n") == 65535);
    CHECK(extractUInt8Field(R"({"n": 255})", "n") == 255);
    CHECK(extractUInt32Field(R"({"n": 4294967295})", "n") == 4294967295u);
}

TEST_CASE("What the escaper writes, the reader reads back unchanged") {
    // The two halves never met: jsonEscape wrote the escapes and nothing undid
    // them, so a device name with a quote in it came back truncated at the
    // backslash and the configuration the next boot restored was not the one
    // that had been saved. Session names arrive straight off the network.
    for (const std::string& original : {
             std::string("My \"Main\" Feed"),
             std::string("two\nlines"),
             std::string("back\\slash"),
             std::string("tab\there"),
             std::string("bell\x07and\x1fnull-adjacent"),
             std::string("plain"),
             std::string(""),
         }) {
        const std::string document = "{\"name\": \"" + jsonEscape(original) + "\"}";
        const auto read = extractStringField(document, "name");
        REQUIRE(read.has_value());
        CHECK(*read == original);
    }
}

TEST_CASE("A field after an escaped quote is still found") {
    // The reader used to stop at the first quote, so everything past an
    // escaped one in an earlier value was a different document to it.
    const std::string json = R"({"name": "a \" b", "port": 5004})";
    CHECK(extractStringField(json, "name") == "a \" b");
    CHECK(extractUInt16Field(json, "port") == 5004);
}

} // namespace Tests
} // namespace AES67
