//
// TestRavennaKitParity.cpp
// aes67-ravenna
//
// This project's SDP against a second implementation's, on the same bytes.
//
// The vectors and every expected value below are ravennakit's own, from
// external/ravennakit/test/ravennakit/sdp/sdp_session_description.test.cpp
// (AGPL-3.0, Sound on Digital; see NOTICE). They are worth taking rather than
// inventing because that suite carries a capture from a real device -- a
// Merging Anubis -- and states field by field what it means. A test written
// here from the same RFCs would agree with this project's reading of them by
// construction, which is the one thing a parity test must not do.
//
// The line numbers cited are that file's, at the pinned v0.22.0.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <string>

#include "Driver/SDPParser.h"

using namespace AES67;

namespace {

// sdp_session_description.test.cpp:60-81. A session an Anubis announces:
// L16 at 48 kHz, two channels, 1 ms packets, a PTP grandmaster named in
// ts-refclk, and a source-filter naming the sender.
constexpr const char* kAnubisSdp =
    "v=0\r\n"
    "o=- 13 0 IN IP4 192.168.15.52\r\n"
    "s=Anubis_610120_13\r\n"
    "c=IN IP4 239.1.15.52/15\r\n"
    "t=0 0\r\n"
    "a=clock-domain:PTPv2 0\r\n"
    "a=ts-refclk:ptp=IEEE1588-2008:00-1D-C1-FF-FE-51-9E-F7:0\r\n"
    "a=mediaclk:direct=0\r\n"
    "m=audio 5004 RTP/AVP 98\r\n"
    "c=IN IP4 239.1.15.52/15\r\n"
    "a=rtpmap:98 L16/48000/2\r\n"
    "a=source-filter: incl IN IP4 239.1.15.52 192.168.15.52\r\n"
    "a=clock-domain:PTPv2 0\r\n"
    "a=sync-time:0\r\n"
    "a=framecount:48\r\n"
    "a=palign:0\r\n"
    "a=ptime:1\r\n"
    "a=ts-refclk:ptp=IEEE1588-2008:00-1D-C1-FF-FE-51-9E-F7:0\r\n"
    "a=mediaclk:direct=0\r\n"
    "a=recvonly\r\n"
    "a=midi-pre2:50040 0,0;0,1\r\n";

} // namespace

TEST_CASE("the Anubis session parses at all") {
    // sdp_session_description.test.cpp:82-83
    const auto parsed = SDPParser::parseString(kAnubisSdp);
    REQUIRE(parsed.has_value());
}

TEST_CASE("the origin line reads the same on both sides") {
    // sdp_session_description.test.cpp:100-108
    const auto parsed = SDPParser::parseString(kAnubisSdp);
    REQUIRE(parsed.has_value());
    CHECK(parsed->originUsername == "-");
    CHECK(parsed->sessionID == 13u);
    CHECK(parsed->sessionVersion == 0u);
    CHECK(parsed->originNetworkType == "IN");
    CHECK(parsed->originAddressType == "IP4");
    CHECK(parsed->originAddress == "192.168.15.52");
}

TEST_CASE("the connection line reads the same on both sides") {
    // sdp_session_description.test.cpp:110-116, and the media-level c= at
    // :141-148, which carries the TTL: 239.1.15.52/15.
    const auto parsed = SDPParser::parseString(kAnubisSdp);
    REQUIRE(parsed.has_value());
    CHECK(parsed->connectionType == "IN");
    CHECK(parsed->connectionNetwork == "IP4");
    CHECK(parsed->connectionAddress == "239.1.15.52");
    CHECK(parsed->ttl == 15);
}

TEST_CASE("the session name and the times read the same on both sides") {
    // sdp_session_description.test.cpp:118-123
    const auto parsed = SDPParser::parseString(kAnubisSdp);
    REQUIRE(parsed.has_value());
    CHECK(parsed->sessionName == "Anubis_610120_13");
    CHECK(parsed->timeStart == 0u);
    CHECK(parsed->timeStop == 0u);
}

TEST_CASE("the media line and its format read the same on both sides") {
    // sdp_session_description.test.cpp:125-140
    const auto parsed = SDPParser::parseString(kAnubisSdp);
    REQUIRE(parsed.has_value());
    CHECK(parsed->mediaType == "audio");
    CHECK(parsed->port == 5004);
    CHECK(parsed->transport == "RTP/AVP");
    CHECK(parsed->payloadType == 98);
    CHECK(parsed->encoding == "L16");
    CHECK(parsed->sampleRate == 48000u);
    CHECK(parsed->numChannels == 2);
}

TEST_CASE("ptime reads the same on both sides") {
    // sdp_session_description.test.cpp:149. One millisecond, which this
    // project keeps in microseconds.
    const auto parsed = SDPParser::parseString(kAnubisSdp);
    REQUIRE(parsed.has_value());
    CHECK(parsed->ptimeUs == 1000u);
}

TEST_CASE("the reference clock reads the same on both sides") {
    // sdp_session_description.test.cpp:151-158
    const auto parsed = SDPParser::parseString(kAnubisSdp);
    REQUIRE(parsed.has_value());
    CHECK(parsed->ptpMasterMAC == "00-1D-C1-FF-FE-51-9E-F7");
    CHECK_FALSE(parsed->ptpTraceable);
}

TEST_CASE("the media clock reads the same on both sides") {
    // sdp_session_description.test.cpp:164-169: direct, offset 0, no rate.
    const auto parsed = SDPParser::parseString(kAnubisSdp);
    REQUIRE(parsed.has_value());
    CHECK(parsed->mediaClockType == "direct=0");
}

TEST_CASE("the source filter reads the same on both sides") {
    // sdp_session_description.test.cpp:171-183. This project keeps the one
    // source the filter names; the destination is the connection address it
    // already has.
    const auto parsed = SDPParser::parseString(kAnubisSdp);
    REQUIRE(parsed.has_value());
    CHECK(parsed->sourceAddress == "192.168.15.52");
}

TEST_CASE("framecount reads the same on both sides") {
    // sdp_session_description.test.cpp:185-187
    const auto parsed = SDPParser::parseString(kAnubisSdp);
    REQUIRE(parsed.has_value());
    CHECK(parsed->framecount == 48u);
}

TEST_CASE("the media direction reads the same on both sides") {
    // sdp_session_description.test.cpp:195-197: a=recvonly, on the media.
    const auto parsed = SDPParser::parseString(kAnubisSdp);
    REQUIRE(parsed.has_value());
    CHECK(parsed->direction == "recvonly");
}

TEST_CASE("both line endings are taken") {
    // sdp_session_description.test.cpp:32-49 feeds three lines -- v=, o=, s=
    // -- with CRLF and then with bare LF, and expects both to parse. This
    // project answers nullopt to both, and for a reason that is not the line
    // ending: parseString() returns a session only when isValid() passes, and
    // isValid() wants a connection address and a media line. The two are not
    // the same function. ravennakit parses and leaves validity to its caller.
    //
    // So the line-ending question is asked here on a session that is complete
    // either way, which is what settles it.
    const std::string crlfOnly = std::string(kAnubisSdp);
    std::string lfOnly = crlfOnly;
    for (std::string::size_type i = lfOnly.find("\r\n"); i != std::string::npos;
         i = lfOnly.find("\r\n", i)) {
        lfOnly.erase(i, 1);
    }
    CHECK(SDPParser::parseString(crlfOnly).has_value());
    CHECK(SDPParser::parseString(lfOnly).has_value());

    // And the difference itself, written down rather than left to be
    // rediscovered: three lines are a session to that parser and not to this
    // one.
    const std::string threeLines =
        "v=0\r\n"
        "o=- 13 0 IN IP4 192.168.15.52\r\n"
        "s=Anubis_610120_13\r\n";
    CHECK_FALSE(SDPParser::parseString(threeLines).has_value());
}

TEST_CASE("what the other implementation refuses is refused here") {
    // sdp_session_description.test.cpp:51-56: a string that is not SDP at all.
    CHECK_FALSE(SDPParser::parseString("bbb").has_value());
}

TEST_CASE("v=1 -- a version SDP does not have -- is refused on both sides") {
    // sdp_session_description.test.cpp:89-96 refuses a session whose version
    // is not 0, the only version RFC 4566 defines. This project took it: the
    // v= line was read and discarded. It is checked now, and this assertion
    // is the one that says so.
    //
    // Asked with a COMPLETE session on purpose. The three-line vector that
    // suite uses would be refused here for an unrelated reason -- see above --
    // and a test that cannot tell the two refusals apart proves nothing.
    const std::string wrongVersion =
        "v=1\r\n"
        "o=- 13 0 IN IP4 192.168.15.52\r\n"
        "s=Anubis_610120_13\r\n"
        "c=IN IP4 239.1.15.52/15\r\n"
        "t=0 0\r\n"
        "m=audio 5004 RTP/AVP 98\r\n"
        "a=rtpmap:98 L16/48000/2\r\n";
    CHECK_FALSE(SDPParser::parseString(wrongVersion).has_value());
}

TEST_CASE("what this project writes, it reads back the same") {
    // Not ravennakit's, but the other half of parity: a round trip through
    // generate() has to survive its own parser, or the comparison above is
    // measuring the parser twice.
    const auto parsed = SDPParser::parseString(kAnubisSdp);
    REQUIRE(parsed.has_value());

    const std::string written = SDPParser::generate(*parsed);
    const auto reread = SDPParser::parseString(written);
    REQUIRE(reread.has_value());

    CHECK(reread->sessionName == parsed->sessionName);
    CHECK(reread->originAddress == parsed->originAddress);
    CHECK(reread->connectionAddress == parsed->connectionAddress);
    CHECK(reread->ttl == parsed->ttl);
    CHECK(reread->port == parsed->port);
    CHECK(reread->payloadType == parsed->payloadType);
    CHECK(reread->encoding == parsed->encoding);
    CHECK(reread->sampleRate == parsed->sampleRate);
    CHECK(reread->numChannels == parsed->numChannels);
    CHECK(reread->ptimeUs == parsed->ptimeUs);
    CHECK(reread->ptpMasterMAC == parsed->ptpMasterMAC);
    CHECK(reread->direction == parsed->direction);
}
