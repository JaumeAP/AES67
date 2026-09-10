//
// TestSourceGen.cpp
// aes67-linux-daemon
// What a profile writes into the daemon's sources, and what it refuses.
//

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "Tools/SourceGen.h"

#include <string>

using namespace AES67;
using namespace AES67::LinuxDriver;

namespace {

SourceResult generate(const std::string& profile,
                      int count = 1,
                      int channels = 2,
                      std::optional<uint32_t> ptime = std::nullopt,
                      std::optional<double> rate = std::nullopt,
                      int startId = 0) {
    SourceRequest request;
    request.kind = CompatibilityProfile::kindFromString(profile);
    request.count = count;
    request.channelsPerSource = channels;
    request.startId = startId;
    request.ptimeUs = ptime;
    request.sampleRate = rate;
    return generateSources(request);
}

bool has(const std::string& json, const std::string& fragment) {
    return json.find(fragment) != std::string::npos;
}

} // namespace

TEST_CASE("AES67 at 48 kHz and 1 ms is 48 samples in a packet") {
    const auto result = generate("aes67");
    REQUIRE(result.ok);
    CHECK(has(result.json, "\"max_samples_per_packet\": 48"));
    CHECK(result.warnings.empty());
}

TEST_CASE("The channel map is consecutive across sources") {
    const auto result = generate("aes67", 3, 2);
    REQUIRE(result.ok);
    CHECK(has(result.json, "\"map\": [ 0, 1 ]"));
    CHECK(has(result.json, "\"map\": [ 2, 3 ]"));
    CHECK(has(result.json, "\"map\": [ 4, 5 ]"));
}

TEST_CASE("Ids start where they are told to") {
    const auto result = generate("aes67", 2, 2, std::nullopt, std::nullopt, 10);
    REQUIRE(result.ok);
    CHECK(has(result.json, "\"id\": 10"));
    CHECK(has(result.json, "\"id\": 11"));
}

TEST_CASE("Ids past 63 are refused") {
    const auto result = generate("aes67", 4, 2, std::nullopt, std::nullopt, 61);
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("0-63") != std::string::npos);
}

TEST_CASE("A packet time between two documented sizes is rounded up, and said to be") {
    const auto result = generate("st2110-30-b");
    REQUIRE(result.ok);
    // 125 us at 48 kHz is six samples, which the daemon does not document.
    CHECK(has(result.json, "\"max_samples_per_packet\": 12"));
    REQUIRE(result.warnings.size() == 1);
    CHECK(result.warnings.front().find("250 us") != std::string::npos);
}

TEST_CASE("Dante writes L24 and EF, which is what its profile says") {
    const auto result = generate("dante");
    REQUIRE(result.ok);
    CHECK(has(result.json, "\"codec\": \"L24\""));
    CHECK(has(result.json, "\"dscp\": 46"));
}

TEST_CASE("Dolby writes its documented multicast address") {
    const auto result = generate("dolby");
    REQUIRE(result.ok);
    CHECK(has(result.json, "\"address\": \"239.81.83.67\""));
}

TEST_CASE("A profile with no documented address leaves it empty") {
    const auto result = generate("aes67");
    REQUIRE(result.ok);
    CHECK(has(result.json, "\"address\": \"\""));
}

TEST_CASE("More channels than the profile carries in a flow is refused") {
    const auto result = generate("aes67", 1, 16);
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("8 channels") != std::string::npos);

    const auto ravenna = generate("ravenna", 1, 16);
    CHECK(ravenna.ok);
}

TEST_CASE("A rate or a packet time outside the profile is refused") {
    const auto rate = generate("dante", 1, 2, std::nullopt, 96000.0);
    CHECK_FALSE(rate.ok);
    CHECK(rate.error.find("96000") != std::string::npos);

    const auto ptime = generate("st2110-30", 1, 2, 125);
    CHECK_FALSE(ptime.ok);
    CHECK(ptime.error.find("125") != std::string::npos);
}

TEST_CASE("Asking for no sources, or a source with no channels, is refused") {
    CHECK_FALSE(generate("aes67", 0).ok);
    CHECK_FALSE(generate("aes67", 1, 0).ok);
}

TEST_CASE("Upstream's own values fill the fields no profile speaks to") {
    const auto result = generate("aes67");
    REQUIRE(result.ok);
    CHECK(has(result.json, "\"ttl\": 15"));
    CHECK(has(result.json, "\"payload_type\": 98"));
    CHECK(has(result.json, "\"io\": \"Audio Device\""));
    CHECK(has(result.json, "\"refclk_ptp_traceable\": true"));
    CHECK(has(result.json, "\"enabled\": true"));
}
