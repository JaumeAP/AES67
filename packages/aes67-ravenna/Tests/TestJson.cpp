//
// TestJson.cpp
// AES67 RAVENNA session layer
// What arrives from a controller, and what goes back.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "Ravenna/Json.h"

using namespace AES67::Ravenna;

TEST_CASE("An IS-05 PATCH body parses into its parts") {
    const std::string body =
        R"({"master_enable":true,)"
        R"("transport_file":{"data":"v=0\r\ns=Mix A\r\n","type":"application/sdp"},)"
        R"("activation":{"mode":"activate_immediate","requested_time":null}})";

    JsonValue value;
    std::string error;
    REQUIRE(parseJson(body, value, error));
    CHECK(error.empty());

    CHECK(value["master_enable"].asBool());
    CHECK(value["transport_file"]["type"].asString() == "application/sdp");
    CHECK(value["transport_file"]["data"].asString() == "v=0\r\ns=Mix A\r\n");
    CHECK(value["activation"]["mode"].asString() == "activate_immediate");
    CHECK(value["activation"]["requested_time"].isNull());
}

TEST_CASE("A member that is not there reads as null instead of throwing") {
    // Every read of a parsed payload goes through this, and a controller
    // sending a partial PATCH is the normal case, not an error.
    JsonValue value;
    std::string error;
    REQUIRE(parseJson(R"({"a":1})", value, error));

    CHECK(value["missing"].isNull());
    CHECK(value["missing"]["deeper"].isNull());
    CHECK(value.has("a"));
    CHECK(value.has("missing") == false);
}

TEST_CASE("What is not JSON is refused, and says where") {
    JsonValue value;
    std::string error;

    CHECK(parseJson("", value, error) == false);
    CHECK(parseJson("{", value, error) == false);
    CHECK(parseJson(R"({"a":})", value, error) == false);
    CHECK(parseJson(R"({"a" 1})", value, error) == false);
    CHECK(parseJson("nul", value, error) == false);
    CHECK(error.empty() == false);

    // Trailing rubbish is a refusal too: a body that parses and then carries
    // something else is not a body this device understood.
    CHECK(parseJson(R"({"a":1} and then some)", value, error) == false);
}

TEST_CASE("Strings keep their escapes on the way in and out") {
    JsonValue value;
    std::string error;
    REQUIRE(parseJson(R"({"sdp":"a=rtpmap\r\n\ttab \"quoted\" back\\slash"})", value, error));
    CHECK(value["sdp"].asString() == "a=rtpmap\r\n\ttab \"quoted\" back\\slash");

    const std::string written = value.serialise();
    JsonValue again;
    REQUIRE(parseJson(written, again, error));
    CHECK(again["sdp"].asString() == value["sdp"].asString());
}

TEST_CASE("A whole number is written as one") {
    // Ports, counts and times go through here. "5004.000000" is a port number
    // nobody wants to read and some controllers refuse.
    CHECK(JsonValue(5004).serialise() == "5004");
    CHECK(JsonValue(0).serialise() == "0");
    CHECK(JsonValue(-1).serialise() == "-1");
    CHECK(JsonValue(1.5).serialise().find("1.5") == 0);
}

TEST_CASE("Objects and arrays go out as they came in") {
    JsonObject inner;
    inner["mode"] = JsonValue("activate_immediate");
    inner["activation_time"] = JsonValue();

    JsonObject outer;
    outer["master_enable"] = JsonValue(true);
    outer["activation"] = JsonValue(inner);
    outer["transport_params"] = JsonValue(JsonArray{JsonValue(JsonObject{})});

    const std::string written = JsonValue(outer).serialise();

    JsonValue parsed;
    std::string error;
    REQUIRE(parseJson(written, parsed, error));
    CHECK(parsed["master_enable"].asBool());
    CHECK(parsed["activation"]["mode"].asString() == "activate_immediate");
    CHECK(parsed["activation"]["activation_time"].isNull());
    REQUIRE(parsed["transport_params"].isArray());
    CHECK(parsed["transport_params"].asArray().size() == 1);
}

TEST_CASE("A control character is escaped rather than written raw") {
    CHECK(jsonQuote(std::string("a\x01" "b")) == "\"a\\u0001b\"");
}
