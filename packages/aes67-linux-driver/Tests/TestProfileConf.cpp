//
// TestProfileConf.cpp
// aes67-linux-driver
// What a profile writes into the daemon's configuration, and what it refuses.
//

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "Tools/ProfileConf.h"

#include <string>

using namespace AES67;
using namespace AES67::LinuxDriver;

namespace {

/// The shape of upstream's daemon.conf: a flat object, one key per line. Only
/// the keys this tool touches, plus two it must leave alone.
const std::string kBase = R"({
  "http_port": 8080,
  "log_severity": 2,
  "tic_frame_size_at_1fs": 48,
  "sample_rate": 48000,
  "rtp_mcast_base": "239.1.0.1",
  "ptp_domain": 0,
  "ptp_dscp": 48,
  "interface_name": "lo"
}
)";

bool has(const std::string& conf, const std::string& line) {
    return conf.find(line) != std::string::npos;
}

ConfResult apply(const std::string& profile,
                 std::optional<double> rate = std::nullopt,
                 std::optional<uint32_t> ptime = std::nullopt,
                 std::optional<std::string> interface = std::nullopt) {
    ConfRequest request;
    request.kind = CompatibilityProfile::kindFromString(profile);
    request.sampleRate = rate;
    request.ptimeUs = ptime;
    request.interfaceName = interface;
    return applyProfile(kBase, request);
}

} // namespace

TEST_CASE("AES67 keeps 48 kHz, 1 ms and domain 0") {
    const auto result = apply("aes67");
    REQUIRE(result.ok);
    CHECK(has(result.text, "\"sample_rate\": 48000"));
    CHECK(has(result.text, "\"tic_frame_size_at_1fs\": 48"));
    CHECK(has(result.text, "\"ptp_domain\": 0"));
}

TEST_CASE("Keys the profile does not determine are left alone") {
    const auto result = apply("aes67");
    REQUIRE(result.ok);
    CHECK(has(result.text, "\"http_port\": 8080"));
    CHECK(has(result.text, "\"log_severity\": 2"));
    // The profile's DSCP is the marking of the media, and this is PTP's.
    CHECK(has(result.text, "\"ptp_dscp\": 48"));
}

TEST_CASE("ST 2110-30 Level B is 125 us, which is six frames at 1FS") {
    const auto result = apply("st2110-30-b");
    REQUIRE(result.ok);
    CHECK(has(result.text, "\"tic_frame_size_at_1fs\": 6"));
}

TEST_CASE("Dolby carries its documented domain and multicast address") {
    const auto result = apply("dolby");
    REQUIRE(result.ok);
    CHECK(has(result.text, "\"ptp_domain\": 109"));
    CHECK(has(result.text, "\"rtp_mcast_base\": \"239.81.83.67\""));
}

TEST_CASE("Dolby at 96 kHz is allowed, and 44.1 is not") {
    const auto ok = apply("dolby", 96000.0);
    REQUIRE(ok.ok);
    CHECK(has(ok.text, "\"sample_rate\": 96000"));

    const auto refused = apply("dolby", 44100.0);
    CHECK_FALSE(refused.ok);
    CHECK(refused.error.find("44100") != std::string::npos);
}

TEST_CASE("A packet time the profile does not allow is refused") {
    const auto result = apply("st2110-30", std::nullopt, 125);
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("125") != std::string::npos);
}

TEST_CASE("A profile that requires a prefix but names no address is refused") {
    const auto result = apply("dante");
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("239.69") != std::string::npos);
}

TEST_CASE("The interface is written only when asked for") {
    const auto untouched = apply("aes67");
    REQUIRE(untouched.ok);
    CHECK(has(untouched.text, "\"interface_name\": \"lo\""));

    const auto named = apply("aes67", std::nullopt, std::nullopt, std::string("eth0"));
    REQUIRE(named.ok);
    CHECK(has(named.text, "\"interface_name\": \"eth0\""));
}

TEST_CASE("A base configuration missing a key is refused, not passed through") {
    ConfRequest request;
    request.kind = CompatibilityProfileKind::AES67;
    const auto result = applyProfile("{\n  \"http_port\": 8080\n}\n", request);
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("sample_rate") != std::string::npos);
}

TEST_CASE("A profile id that is not one of ours is not silently AES67") {
    CHECK(isKnownProfileName("aes67"));
    CHECK(isKnownProfileName("dolby-lan"));
    CHECK_FALSE(isKnownProfileName("aes-67"));
    CHECK_FALSE(isKnownProfileName(""));
}
