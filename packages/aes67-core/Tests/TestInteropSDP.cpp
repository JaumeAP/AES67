//
// TestInteropSDP.cpp
// The SDP parser against a working implementation, not against the RFC.
//
// TestSDPParser covers what RFC 4566 says. This suite covers what
// aes67-linux-daemon actually emits, which is not the same test: a parser can
// agree with a careful reading of the standard and still refuse a description
// that real devices exchange every day. That happened here -- the parser
// rejected the bare-domain form of `a=ts-refclk` that this daemon writes, and
// the fix is in this repository's history.
//
// One extraction pitfall, recorded because it cost a failing run: the daemon's
// latency test carries an SDP *template* with bare uppercase placeholders --
// 239.1.0.ADDR, CODEC/SR/2, PTIME. It looks like a session description and is
// not one. The parser was right to refuse it. Fixtures here are only documents
// a program actually emitted or accepted.
//
// So the assertion is deliberately shallow and broad: every fixture parses,
// and the fields any receiver must act on come out right. Depth belongs in
// TestSDPParser; this is about not being surprised by the world.
//

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "Driver/SDPParser.h"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string readFixture(const std::string& name) {
    const std::string path = std::string(AES67_FIXTURE_DIR) + "/linux-daemon/" + name;
    std::ifstream in(path);
    REQUIRE_MESSAGE(in.good(), "fixture not found: " << path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

const std::vector<std::string> kFixtures = {
    "daemon-01.sdp", "daemon-02.sdp", "daemon-03.sdp", "daemon-04.sdp",
    "daemon-05.sdp", "daemon-06.sdp",
};

}  // namespace

TEST_CASE("Every SDP the Linux daemon produces parses") {
    for (const auto& name : kFixtures) {
        CAPTURE(name);
        const std::string sdp = readFixture(name);
        const auto session = AES67::SDPParser::parseString(sdp);
        CHECK(session.has_value());
    }
}

TEST_CASE("The fields a receiver has to act on survive the parse") {
    for (const auto& name : kFixtures) {
        CAPTURE(name);
        const auto parsed = AES67::SDPParser::parseString(readFixture(name));
        REQUIRE(parsed.has_value());
        const AES67::SDPSession& session = *parsed;

        // Without these four a receiver cannot open a stream at all.
        CHECK(session.connectionAddress.empty() == false);
        CHECK(session.port > 0);
        CHECK(session.sampleRate > 0);
        CHECK(session.numChannels > 0);

        // AES67 is L16 or L24 over dynamic payload types.
        CHECK((session.encoding == "L16" || session.encoding == "L24"));
        CHECK(session.payloadType >= 96);
        CHECK(session.payloadType <= 127);
    }
}

TEST_CASE("Re-generating a parsed session produces something parseable") {
    // A parser that reads a description and writes one that cannot be read
    // back is half a parser, and the failure only shows when another device
    // reads it.
    for (const auto& name : kFixtures) {
        CAPTURE(name);
        const auto originalParsed = AES67::SDPParser::parseString(readFixture(name));
        REQUIRE(originalParsed.has_value());
        const AES67::SDPSession& original = *originalParsed;

        const auto roundParsed = AES67::SDPParser::parseString(AES67::SDPParser::generate(original));
        REQUIRE(roundParsed.has_value());
        const AES67::SDPSession& round = *roundParsed;

        CHECK(round.connectionAddress == original.connectionAddress);
        CHECK(round.port == original.port);
        CHECK(round.sampleRate == original.sampleRate);
        CHECK(round.numChannels == original.numChannels);
        CHECK(round.encoding == original.encoding);
        CHECK(round.payloadType == original.payloadType);
    }
}
