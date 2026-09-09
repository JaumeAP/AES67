//
// TestConfCheck.cpp
// aes67-linux-driver
// What --check reports, and on whose authority.
//

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "Tools/ConfCheck.h"

#include <string>

using namespace AES67;
using namespace AES67::LinuxDriver;

namespace {

/// A configuration with nothing wrong in it: upstream's own values, an AES67
/// rate and packet time, and every path present.
const std::string kClean = R"({
  "http_port": 8080,
  "rtsp_port": 8854,
  "http_base_dir": "../webui/dist",
  "log_severity": 2,
  "playout_delay": 0,
  "tic_frame_size_at_1fs": 48,
  "max_tic_frame_size": 1024,
  "sample_rate": 48000,
  "rtp_mcast_base": "239.1.0.1",
  "rtp_mcast_base_sec": "239.1.0.1",
  "rtp_port": 5004,
  "rtp_port_sec": 5004,
  "ptp_domain": 0,
  "ptp_dscp": 48,
  "sap_mcast_addr": "239.255.255.255",
  "sap_interval": 30,
  "syslog_proto": "none",
  "status_file": "./status.json",
  "interface_name": "eth0",
  "ptp_status_script": "./scripts/ptp_status.sh",
  "nmos_node_port": 3218,
  "nmos_registry_port": 3210
}
)";

const PathProbe kEverythingExists = [](const std::string&) { return true; };
const PathProbe kNothingExists = [](const std::string&) { return false; };

std::string with(const std::string& key, const std::string& value) {
    std::string conf = kClean;
    const auto keyPos = conf.find("\"" + key + "\"");
    REQUIRE(keyPos != std::string::npos);
    const auto colon = conf.find(':', keyPos);
    const auto end = conf.find('\n', colon);
    conf.replace(colon + 1, end - (colon + 1), " " + value + ",");
    return conf;
}

std::vector<Finding> check(const std::string& conf,
                           std::optional<CompatibilityProfileKind> kind = std::nullopt,
                           const PathProbe& exists = kEverythingExists) {
    return checkConf(conf, kind, exists);
}

bool reports(const std::vector<Finding>& findings, Severity severity, const std::string& key) {
    for (const auto& finding : findings) {
        if (finding.severity == severity && finding.key == key) return true;
    }
    return false;
}

} // namespace

TEST_CASE("A clean configuration reports nothing") {
    CHECK(check(kClean).empty());
}

TEST_CASE("A rate the profile forbids is an error") {
    const auto findings = check(with("sample_rate", "96000"),
                                CompatibilityProfileKind::ST2110_30);
    CHECK(reports(findings, Severity::Error, "sample_rate"));
}

TEST_CASE("A frame size the profile forbids is an error, read as a packet time") {
    const auto findings = check(with("tic_frame_size_at_1fs", "6"),
                                CompatibilityProfileKind::AES67);
    REQUIRE(reports(findings, Severity::Error, "tic_frame_size_at_1fs"));
    CHECK(findings.front().message.find("125 us") != std::string::npos);
}

TEST_CASE("A fixed domain is an error and a documented one is a warning") {
    const auto dante = check(with("ptp_domain", "5"), CompatibilityProfileKind::Dante);
    CHECK(reports(dante, Severity::Error, "ptp_domain"));

    const auto dolby = check(kClean, CompatibilityProfileKind::Dolby);
    CHECK(reports(dolby, Severity::Warning, "ptp_domain"));
}

TEST_CASE("Multicast outside a profile's required prefix is an error") {
    const auto findings = check(kClean, CompatibilityProfileKind::Dante);
    CHECK(reports(findings, Severity::Error, "rtp_mcast_base"));
    CHECK(reports(findings, Severity::Error, "rtp_mcast_base_sec"));
}

TEST_CASE("An address that is not multicast is an error") {
    const auto findings = check(with("rtp_mcast_base", "\"192.168.1.50\""));
    CHECK(reports(findings, Severity::Error, "rtp_mcast_base"));
}

TEST_CASE("A SAP address that is neither of RFC 2974's is a warning") {
    const auto findings = check(with("sap_mcast_addr", "\"239.1.2.3\""));
    CHECK(reports(findings, Severity::Warning, "sap_mcast_addr"));
}

TEST_CASE("An odd RTP port is an error") {
    const auto findings = check(with("rtp_port", "5005"));
    CHECK(reports(findings, Severity::Error, "rtp_port"));
}

TEST_CASE("A PTP domain above 127 and a DSCP above 63 are errors") {
    CHECK(reports(check(with("ptp_domain", "200")), Severity::Error, "ptp_domain"));
    CHECK(reports(check(with("ptp_dscp", "64")), Severity::Error, "ptp_dscp"));
}

TEST_CASE("A frame size above max_tic_frame_size is an error, and zero is too") {
    CHECK(reports(check(with("tic_frame_size_at_1fs", "2048")), Severity::Error,
                  "tic_frame_size_at_1fs"));
    CHECK(reports(check(with("tic_frame_size_at_1fs", "0")), Severity::Error,
                  "tic_frame_size_at_1fs"));
}

TEST_CASE("Two servers on one port is an error") {
    const auto findings = check(with("rtsp_port", "8080"));
    CHECK(reports(findings, Severity::Error, "rtsp_port"));
}

TEST_CASE("A value too big for the type the daemon reads is an error") {
    const auto findings = check(with("sap_interval", "70000"));
    CHECK(reports(findings, Severity::Error, "sap_interval"));
}

TEST_CASE("A rate the project does not test is a warning") {
    const auto findings = check(with("sample_rate", "50000"));
    CHECK(reports(findings, Severity::Warning, "sample_rate"));
}

TEST_CASE("A syslog protocol the daemon does not name is a warning") {
    const auto findings = check(with("syslog_proto", "\"sctp\""));
    CHECK(reports(findings, Severity::Warning, "syslog_proto"));
}

TEST_CASE("A log severity above fatal is a warning") {
    const auto findings = check(with("log_severity", "9"));
    CHECK(reports(findings, Severity::Warning, "log_severity"));
}

TEST_CASE("A key the daemon does not read is a warning") {
    std::string conf = kClean;
    conf.insert(conf.find("\"http_port\""), "\"http_prot\": 9000,\n  ");
    const auto findings = check(conf);
    CHECK(reports(findings, Severity::Warning, "http_prot"));
}

TEST_CASE("A path that is not there is a warning, and only that") {
    const auto findings = check(kClean, std::nullopt, kNothingExists);
    CHECK(reports(findings, Severity::Warning, "http_base_dir"));
    CHECK(reports(findings, Severity::Warning, "status_file"));
    CHECK(reports(findings, Severity::Warning, "ptp_status_script"));
    CHECK_FALSE(hasError(findings));
}

TEST_CASE("A file with no keys at all is an error, not an empty report") {
    const auto findings = check("nothing here\n");
    REQUIRE(findings.size() == 1);
    CHECK(findings.front().severity == Severity::Error);
}
