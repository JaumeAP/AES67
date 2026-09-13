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
}

} // namespace Tests
} // namespace AES67
