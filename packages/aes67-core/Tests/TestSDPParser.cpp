//
// TestSDPParser.cpp
// AES67 macOS Driver - Build #4
// Unit tests for SDP Parser
//

#include "Driver/SDPParser.h"
#include "NetworkEngine/RTP/RTPHeader.h"
#include <iostream>
#include <fstream>
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <string>
#include <sstream>
#include <vector>
#include <cstdio>
#include <cstdlib>

namespace AES67 {
namespace Tests {

TEST_CASE("Basic SDP Parsing") {
    std::cout << "Test: Basic SDP Parsing... ";

    std::string sdp = R"(v=0
o=- 1729346400 0 IN IP4 192.168.1.100
s=Test Stream
i=8 Channel Test
t=0 0
m=audio 5004 RTP/AVP 96
c=IN IP4 239.69.83.171/32
a=rtpmap:96 L24/48000/8
a=ptime:1
a=framecount:48
)";

    auto session = SDPParser::parseString(sdp);
    CHECK(session.has_value());;
    CHECK(session->sessionName == "Test Stream");;
    CHECK(session->sessionInfo == "8 Channel Test");;
    CHECK(session->connectionAddress == "239.69.83.171");;
    CHECK(session->port == 5004);;
    CHECK(session->sampleRate == 48000);;
    CHECK(session->numChannels == 8);;
    CHECK(session->encoding == "L24");;
    CHECK(session->ptimeUs == 1000);;
    CHECK(session->framecount == 48);;

    std::cout << "✓ PASSED\n";
}

TEST_CASE("Riedel Compatible SDP") {
    std::cout << "Test: Riedel Artist SDP Parsing... ";

    std::string sdp = R"(v=0
o=- 1729346400 0 IN IP4 192.168.1.100
s=Riedel Artist IFB
i=Intercom Feed Back 8 Channels
t=0 0
a=clock-domain:PTPv2 0
a=recvonly
m=audio 5004 RTP/AVP 96
c=IN IP4 239.69.83.171/32
a=rtpmap:96 L24/48000/8
a=ptime:1
a=framecount:48
a=source-filter: incl IN IP4 239.69.83.171 192.168.1.100
a=ts-refclk:ptp=IEEE1588-2008:00-1B-21-AC-B5-4F:domain-nmbr=0
a=mediaclk:direct=0
)";

    auto session = SDPParser::parseString(sdp);
    CHECK(session.has_value());;
    CHECK(session->sessionName == "Riedel Artist IFB");;
    CHECK(session->ptpDomain == 0);;
    CHECK(session->ptpMasterMAC == "00-1B-21-AC-B5-4F");;
    CHECK(session->sourceAddress == "192.168.1.100");;

    std::cout << "✓ PASSED\n";
}

TEST_CASE("L16 Encoding") {
    std::cout << "Test: L16 Encoding... ";

    std::string sdp = R"(v=0
o=- 1729346400 0 IN IP4 192.168.1.100
s=L16 Test
t=0 0
m=audio 5004 RTP/AVP 96
c=IN IP4 239.69.83.1/32
a=rtpmap:96 L16/48000/2
a=ptime:1
)";

    auto session = SDPParser::parseString(sdp);
    CHECK(session.has_value());;
    CHECK(session->encoding == "L16");;
    CHECK(session->numChannels == 2);;

    std::cout << "✓ PASSED\n";
}

TEST_CASE("High Sample Rates") {
    std::cout << "Test: High Sample Rates (96kHz, 192kHz)... ";

    // Test 96kHz
    std::string sdp96 = R"(v=0
o=- 1729346400 0 IN IP4 192.168.1.100
s=96kHz Test
t=0 0
m=audio 5004 RTP/AVP 96
c=IN IP4 239.69.83.1/32
a=rtpmap:96 L24/96000/8
a=ptime:0.5
a=framecount:48
)";

    auto session96 = SDPParser::parseString(sdp96);
    CHECK(session96.has_value());;
    CHECK(session96->sampleRate == 96000);;
    // a=ptime:0.5 — sub-millisecond, and legal SDP. This used to parse to
    // zero: ptime was held as integer milliseconds and read with stoul,
    // which stops at the decimal point. The test file has carried these
    // fractional values since before that was noticed, but only ever
    // asserted the sample rate.
    CHECK(session96->ptimeUs == 500);;

    // Test 192kHz
    std::string sdp192 = R"(v=0
o=- 1729346400 0 IN IP4 192.168.1.100
s=192kHz Test
t=0 0
m=audio 5004 RTP/AVP 96
c=IN IP4 239.69.83.1/32
a=rtpmap:96 L24/192000/8
a=ptime:0.25
a=framecount:48
)";

    auto session192 = SDPParser::parseString(sdp192);
    CHECK(session192.has_value());;
    CHECK(session192->sampleRate == 192000);;
    CHECK(session192->ptimeUs == 250);;

    // ST 2110-30 Levels B and C run at 125 us — the value this driver's
    // transmitter can now express, and the reason packet time is held in
    // microseconds at all.
    std::string sdp125 = R"(v=0
o=- 1729346400 0 IN IP4 192.168.1.100
s=125us Test
t=0 0
m=audio 5004 RTP/AVP 96
c=IN IP4 239.69.83.1/32
a=rtpmap:96 L24/48000/8
a=ptime:0.125
a=framecount:6
)";
    auto session125 = SDPParser::parseString(sdp125);
    CHECK(session125.has_value());;
    CHECK(session125->ptimeUs == 125);;

    // Round trip: a fractional packet time must survive being written back
    // out as SDP, not be rounded to "0" or "1".
    std::string regenerated = SDPParser::generate(*session125);
    CHECK(regenerated.find("a=ptime:0.125") != std::string::npos);;
    auto reparsed = SDPParser::parseString(regenerated);
    CHECK(reparsed.has_value());;
    CHECK(reparsed->ptimeUs == 125);;

    // A whole millisecond must still be written the plain way every other
    // implementation writes it, not as "1.000".
    auto whole = SDPParser::parseString(sdp96);
    CHECK(whole.has_value());;
    whole->ptimeUs = 1000;
    CHECK(SDPParser::generate(*whole).find("a=ptime:1\r\n") != std::string::npos);;

    std::cout << "✓ PASSED\n";
}

TEST_CASE("Multi Channel Configurations") {
    std::cout << "Test: Multi-Channel Configurations... ";

    // Test 64 channels
    std::string sdp64 = R"(v=0
o=- 1729346400 0 IN IP4 192.168.1.100
s=64 Channel Test
t=0 0
m=audio 5004 RTP/AVP 96
c=IN IP4 239.69.83.1/32
a=rtpmap:96 L24/48000/64
a=ptime:1
)";

    auto session64 = SDPParser::parseString(sdp64);
    CHECK(session64.has_value());;
    CHECK(session64->numChannels == 64);;

    std::cout << "✓ PASSED\n";
}

TEST_CASE("SDP Generation") {
    std::cout << "Test: SDP Generation... ";

    SDPSession session;
    session.sessionName = "Generated Stream";
    session.sessionInfo = "Test Description";
    session.originAddress = "192.168.1.200";
    session.connectionAddress = "239.69.100.1";
    session.port = 5008;
    session.sampleRate = 48000;
    session.numChannels = 8;
    session.encoding = "L24";
    session.ptimeUs = 1000;
    session.framecount = 48;
    session.ptpDomain = 0;

    std::string generated = SDPParser::generate(session);
    CHECK(!generated.empty());;

    // The origin line in RFC 4566's order: nettype "IN" before addrtype
    // "IP4", then the address. The defaults were the other way round, and
    // only a session built here rather than parsed showed it.
    CHECK((generated.rfind("o=- ", 0) == 0 || generated.find("\no=- ") != std::string::npos));
    CHECK(generated.find(" IN IP4 192.168.1.200\r\n") != std::string::npos);
    CHECK(generated.find("IP4 IN") == std::string::npos);

    // The connection address twice: once at session level and once inside the
    // media description. RFC 4566 sec 5.7 allows both and lets the per-media
    // line override, and readers are split on which one they look at -- the
    // AMWA IS-05 suite reads only the media section, most AES67 gear only the
    // session level. Written once, one of the two found nothing.
    const size_t sessionLevel = generated.find("c=IN IP4 239.69.100.1");
    CHECK(sessionLevel != std::string::npos);
    const size_t mediaLine = generated.find("m=audio ");
    CHECK(mediaLine != std::string::npos);
    CHECK(sessionLevel < mediaLine);
    CHECK(generated.find("c=IN IP4 239.69.100.1", mediaLine) != std::string::npos);

    // Verify it can be parsed back
    auto reparsed = SDPParser::parseString(generated);
    CHECK(reparsed.has_value());;
    CHECK(reparsed->sessionName == session.sessionName);;
    CHECK(reparsed->connectionAddress == session.connectionAddress);;
    CHECK(reparsed->port == session.port);;

    std::cout << "✓ PASSED\n";
}

TEST_CASE("PTP Traceable Ref Clock") {
    std::cout << "Test: PTP traceable ts-refclk (RFC 7273)... ";

    // Parse the traceable form: no gmid, no domain pinned.
    std::string sdp = R"(v=0
o=- 1729346400 0 IN IP4 192.168.1.100
s=Traceable GM
t=0 0
m=audio 5004 RTP/AVP 97
c=IN IP4 239.69.83.1/32
a=rtpmap:97 L24/48000/8
a=ptime:1
a=ts-refclk:ptp=IEEE1588-2008:traceable
a=mediaclk:direct=0
)";
    auto session = SDPParser::parseString(sdp);
    CHECK(session.has_value());;
    CHECK(session->ptpTraceable);;
    CHECK(session->ptpMasterMAC.empty());;

    // Regenerate: must emit the traceable form, not a named grandmaster.
    std::string gen = SDPParser::generate(*session);
    CHECK(gen.find("a=ts-refclk:ptp=IEEE1588-2008:traceable") != std::string::npos);;
    CHECK(gen.find("domain-nmbr=") == std::string::npos);;

    // And it round-trips back to traceable.
    auto reparsed = SDPParser::parseString(gen);
    CHECK(reparsed.has_value());;
    CHECK(reparsed->ptpTraceable);;

    // A named grandmaster still generates the gmid+domain form (traceable
    // false), and traceable takes precedence when both are somehow set.
    SDPSession named;
    named.sessionName = "Named GM";
    named.originAddress = "192.168.1.200";
    named.connectionAddress = "239.69.100.1";
    named.port = 5008;
    named.encoding = "L24";
    named.ptpDomain = 0;
    named.ptpMasterMAC = "00-1B-21-AC-B5-4F";
    std::string namedGen = SDPParser::generate(named);
    CHECK(namedGen.find("00-1B-21-AC-B5-4F") != std::string::npos);;
    CHECK(namedGen.find("traceable") == std::string::npos);;

    std::cout << "✓ PASSED\n";
}

TEST_CASE("Bare Domain Ref Clock") {
    std::cout << "Test: RFC 7273 bare-domain ts-refclk (aes67-linux-daemon form)... ";
    // The daemon emits ptp=IEEE1588-2008:<gmid>:<domain> — a bare number, not
    // ":domain-nmbr=". This must parse (it used to reject the whole SDP).
    std::string sdp = R"(v=0
o=- 1 1 IN IP4 192.168.1.50
s=daemon
t=0 0
m=audio 5004 RTP/AVP 98
c=IN IP4 239.69.83.10/32
a=rtpmap:98 L24/48000/8
a=ptime:1
a=ts-refclk:ptp=IEEE1588-2008:00-11-22-33-44-55-66-77:0
)";
    auto session = SDPParser::parseString(sdp);
    CHECK(session.has_value());;
    CHECK(session->ptpDomain == 0);;
    CHECK(session->ptpMasterMAC == "00-11-22-33-44-55-66-77");;

    // Non-zero bare domain too.
    std::string sdp9 = R"(v=0
o=- 1 1 IN IP4 192.168.1.50
s=daemon
t=0 0
m=audio 5004 RTP/AVP 98
c=IN IP4 239.69.83.10/32
a=rtpmap:98 L24/48000/8
a=ptime:1
a=ts-refclk:ptp=IEEE1588-2008:00-1B-21-AC-B5-4F:9
)";
    auto s9 = SDPParser::parseString(sdp9);
    CHECK(s9.has_value());;
    CHECK(s9->ptpDomain == 9);;

    // The domain-nmbr= variant must still work.
    std::string sdpN = R"(v=0
o=- 1 1 IN IP4 192.168.1.50
s=daemon
t=0 0
m=audio 5004 RTP/AVP 98
c=IN IP4 239.69.83.10/32
a=rtpmap:98 L24/48000/8
a=ptime:1
a=ts-refclk:ptp=IEEE1588-2008:00-1B-21-AC-B5-4F:domain-nmbr=3
)";
    auto sN = SDPParser::parseString(sdpN);
    CHECK((sN.has_value() && sN->ptpDomain == 3));

    // And what we write is the bare form: Dante Controller reads the domain
    // with Integer.parseInt, and "domain-nmbr=0" is not a number to it.
    SDPSession ours;
    ours.sessionName = "ours";
    ours.originAddress = "192.168.1.60";
    ours.connectionAddress = "239.69.83.20";
    ours.port = 5004;
    ours.ptpMasterMAC = "00-60-2B-FF-FE-11-22-33";
    ours.ptpDomain = 0;
    const std::string gen = SDPParser::generate(ours);
    CHECK(gen.find("a=ts-refclk:ptp=IEEE1588-2008:00-60-2B-FF-FE-11-22-33:0\r\n") != std::string::npos);
    CHECK(gen.find("domain-nmbr=") == std::string::npos);
    auto back = SDPParser::parseString(gen);
    CHECK((back.has_value() && back->ptpDomain == 0 && back->ptpMasterMAC == ours.ptpMasterMAC));

    std::cout << "\u2713 PASSED\n";
}

TEST_CASE("Invalid SDP") {
    std::cout << "Test: Invalid SDP Handling... ";

    // Empty SDP
    auto empty = SDPParser::parseString("");
    CHECK(!empty.has_value());;

    // Missing required fields
    std::string incomplete = R"(v=0
s=Incomplete
)";
    auto inc = SDPParser::parseString(incomplete);
    CHECK(!inc.has_value());;

    std::cout << "✓ PASSED\n";
}

TEST_CASE("File Operations") {
    std::cout << "Test: File Operations... ";

    // Create test SDP file
    std::string testPath = "test_aes67_tmp.sdp"; // CWD, not /tmp (portable / sandbox-safe)
    std::string sdp = R"(v=0
o=- 1729346400 0 IN IP4 192.168.1.100
s=File Test
t=0 0
m=audio 5004 RTP/AVP 96
c=IN IP4 239.69.83.1/32
a=rtpmap:96 L24/48000/8
)";

    // Write test file
    std::ofstream out(testPath);
    out << sdp;
    out.close();

    // Parse from file
    auto session = SDPParser::parseFile(testPath);
    CHECK(session.has_value());;
    CHECK(session->sessionName == "File Test");;

    // Cleanup
    std::remove(testPath.c_str());

    std::cout << "✓ PASSED\n";
}

// runAllTests() and main() are gone: doctest registers every TEST_CASE above
// and provides the runner via DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN.

} // namespace Tests
} // namespace AES67

namespace AES67 {
namespace Tests {
TEST_CASE("Generated SDP states the PTP clock domain") {
    // RAVENNA's a=clock-domain, which the AES67 Linux daemon writes and this
    // generator used to leave out: with ts-refclk in its traceable form no
    // domain is pinned anywhere else in the session.
    SDPSession session;
    session.sessionName = "Domain";
    session.originAddress = "192.168.1.50";
    session.connectionAddress = "239.1.0.1";
    session.ptpDomain = 109;
    session.ptpTraceable = true;

    const std::string sdp = SDPParser::generate(session);
    CHECK(sdp.find("a=clock-domain:PTPv2 109\r\n") != std::string::npos);

    SUBCASE("and does not repeat one the parsed session already carried") {
        const auto parsed = SDPParser::parseString(sdp);
        REQUIRE(parsed.has_value());
        const std::string again = SDPParser::generate(*parsed);
        const auto first = again.find("a=clock-domain:");
        REQUIRE(first != std::string::npos);
        CHECK(again.find("a=clock-domain:", first + 1) == std::string::npos);
    }

    SUBCASE("and states none when the session has no PTP domain") {
        session.ptpDomain = -1;
        CHECK(SDPParser::generate(session).find("a=clock-domain:") == std::string::npos);
    }
}

// ===========================================================================
// The parts of SDPParser nothing reached: the validation report, the file
// ends, the session builders, and the refusal paths in parseString.
// ===========================================================================

TEST_CASE("The validation report names every missing field, not just the first") {
    SDPSession session;
    session.sessionName.clear();
    session.connectionAddress.clear();
    session.port = 0;
    session.encoding = "MP3";
    session.sampleRate = 0;
    session.numChannels = 0;

    const auto errors = session.getValidationErrors();
    CHECK_FALSE(session.isValid());
    REQUIRE(errors.size() == 6);
    CHECK(errors[0] == "Session name (s=) is required");
    CHECK(errors[1] == "Connection address (c=) is required");
    CHECK(errors[2] == "Port must be non-zero");
    CHECK(errors[3] == "Invalid encoding: MP3");
    CHECK(errors[4] == "Sample rate must be non-zero");
    CHECK(errors[5] == "Channel count must be non-zero");
}

TEST_CASE("The three AES67 encodings pass validation and nothing else does") {
    SDPSession session;
    session.sessionName = "Studio A";
    session.connectionAddress = "239.69.0.1";

    for (const std::string& encoding : {"L16", "L24", "AM824"}) {
        session.encoding = encoding;
        CHECK(session.isValid());
    }

    session.encoding = "L32";
    CHECK_FALSE(session.isValid());
    session.encoding.clear();
    CHECK_FALSE(session.isValid());
}

TEST_CASE("validate reports through the vector it was handed, or through neither") {
    SDPSession good;
    good.sessionName = "Studio A";
    good.connectionAddress = "239.69.0.1";

    std::vector<std::string> errors{"stale entry"};
    CHECK(SDPParser::validate(good, &errors));
    CHECK(errors.empty());          // the vector is replaced, not appended to
    CHECK(SDPParser::validate(good));   // and the pointer is optional

    SDPSession bad;                 // no session name, no connection address
    CHECK_FALSE(SDPParser::validate(bad, &errors));
    CHECK(errors.size() == 2);
    CHECK_FALSE(SDPParser::validate(bad));
}

TEST_CASE("A file that is not there parses to nothing") {
    CHECK_FALSE(SDPParser::parseFile("/nonexistent-directory-aes67/stream.sdp").has_value());
}

TEST_CASE("A session written to a file parses back out of it") {
    const std::string path =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
        "/aes67_test_roundtrip.sdp";
    (void)std::remove(path.c_str());

    const SDPSession original = SDPParser::createDefaultTxSession(
        "Studio A", "192.168.1.100", "239.69.83.171", 5004, 8, 48000, "L24");
    REQUIRE(SDPParser::writeFile(original, path));

    const auto parsed = SDPParser::parseFile(path);
    REQUIRE(parsed.has_value());
    CHECK(parsed->sessionName == "Studio A");
    CHECK(parsed->connectionAddress == "239.69.83.171");
    CHECK(parsed->port == 5004);
    CHECK(parsed->encoding == "L24");
    CHECK(parsed->sampleRate == 48000);
    CHECK(parsed->numChannels == 8);

    (void)std::remove(path.c_str());
}

TEST_CASE("A file that cannot be opened for writing is reported, not thrown") {
    const SDPSession session = SDPParser::createDefaultTxSession(
        "Studio A", "192.168.1.100", "239.69.83.171", 5004, 2, 48000);
    CHECK_FALSE(SDPParser::writeFile(session, "/nonexistent-directory-aes67/stream.sdp"));
}

TEST_CASE("A default transmit session is complete enough to be valid") {
    const SDPSession session = SDPParser::createDefaultTxSession(
        "Studio A", "192.168.1.100", "239.69.83.171", 5004, 8, 96000, "L16");

    CHECK(session.isValid());
    CHECK(session.sessionName == "Studio A");
    CHECK(session.sessionInfo == "AES67 Stream");
    CHECK(session.sessionVersion == 0);
    CHECK(session.sessionID != 0);
    CHECK(session.originUsername == "-");
    CHECK(session.originAddress == "192.168.1.100");
    CHECK(session.originNetworkType == "IN");
    CHECK(session.originAddressType == "IP4");
    CHECK(session.connectionAddress == "239.69.83.171");
    CHECK(session.ttl == 32);
    CHECK(session.timeStart == 0);
    CHECK(session.timeStop == 0);
    CHECK(session.mediaType == "audio");
    CHECK(session.port == 5004);
    CHECK(session.transport == "RTP/AVP");
    CHECK(session.encoding == "L16");
    CHECK(session.sampleRate == 96000);
    CHECK(session.numChannels == 8);
    CHECK(session.direction == "sendonly");
    CHECK(session.sourceAddress == "192.168.1.100");
    CHECK(session.ptpDomain == 0);
    CHECK(session.mediaClockType == "direct=0");

    // 1 ms packets, and the frame count that goes with them at this rate.
    CHECK(session.ptimeUs == 1000);
    CHECK(session.framecount == 96);
}

TEST_CASE("The default transmit session's payload type follows its encoding") {
    const SDPSession wide = SDPParser::createDefaultTxSession(
        "A", "192.168.1.100", "239.69.0.1", 5004, 2, 48000, "L24");
    const SDPSession narrow = SDPParser::createDefaultTxSession(
        "A", "192.168.1.100", "239.69.0.1", 5004, 2, 48000, "L16");

    CHECK(wide.payloadType == RTP::payloadTypeFor("L24"));
    CHECK(narrow.payloadType == RTP::payloadTypeFor("L16"));
}

TEST_CASE("A session becomes the stream description the engine works from") {
    SDPSession session = SDPParser::createDefaultTxSession(
        "Studio A", "192.168.1.100", "239.69.83.171", 5004, 8, 48000, "L24");
    session.sessionInfo = "8 channels from the live room";
    session.ttl = 16;
    session.ptimeUs = 125;
    session.framecount = 6;
    session.ptpMasterMAC = "00-1D-C1-FF-FE-12-34-56";
    session.ptpDomain = 127;

    const StreamInfo info = SDPParser::toStreamInfo(session);

    CHECK_FALSE(info.id.isNull());          // a fresh identifier per call
    CHECK(info.name == "Studio A");
    CHECK(info.description == "8 channels from the live room");
    CHECK(info.source.ip == "192.168.1.100");
    CHECK(info.source.port == 0);           // an SDP does not carry one
    CHECK(info.multicast.ip == "239.69.83.171");
    CHECK(info.multicast.port == 5004);
    CHECK(info.multicast.ttl == 16);
    CHECK(info.encoding == AudioEncoding::L24);
    CHECK(info.sampleRate == 48000);
    CHECK(info.numChannels == 8);
    CHECK(info.payloadType == session.payloadType);
    CHECK(info.ptime == 125);
    CHECK(info.framecount == 6);
    CHECK(info.ptp.domain == 127);
    CHECK(info.ptp.masterMAC == "00-1D-C1-FF-FE-12-34-56");
    CHECK(info.ptp.enabled);
}

TEST_CASE("Two sessions converted in turn get identifiers of their own") {
    const SDPSession session = SDPParser::createDefaultTxSession(
        "Studio A", "192.168.1.100", "239.69.0.1", 5004, 2, 48000);
    CHECK(SDPParser::toStreamInfo(session).id != SDPParser::toStreamInfo(session).id);
}

TEST_CASE("An encoding the engine has no format for becomes Unknown") {
    SDPSession session = SDPParser::createDefaultTxSession(
        "Studio A", "192.168.1.100", "239.69.0.1", 5004, 2, 48000, "L16");
    CHECK(SDPParser::toStreamInfo(session).encoding == AudioEncoding::L16);

    session.encoding = "AM824";
    CHECK(SDPParser::toStreamInfo(session).encoding == AudioEncoding::Unknown);
}

TEST_CASE("A negative PTP domain converts to PTP being switched off") {
    SDPSession session = SDPParser::createDefaultTxSession(
        "Studio A", "192.168.1.100", "239.69.0.1", 5004, 2, 48000);
    session.ptpDomain = -1;
    CHECK_FALSE(SDPParser::toStreamInfo(session).ptp.enabled);
}

TEST_CASE("A stream description becomes a session again") {
    StreamInfo info;
    info.id = StreamID::generate();
    info.name = "Studio B";
    info.description = "the other room";
    info.source.ip = "192.168.1.101";
    info.multicast = NetworkAddress{"239.69.83.172", 5006};
    info.multicast.ttl = 8;
    info.encoding = AudioEncoding::L16;
    info.sampleRate = 44100;
    info.numChannels = 2;
    info.payloadType = 98;
    info.ptime = 250;
    info.framecount = 11;
    info.ptp.domain = 42;
    info.ptp.masterMAC = "00-1D-C1-FF-FE-12-34-56";

    const SDPSession session = SDPParser::fromStreamInfo(info);

    CHECK(session.sessionName == "Studio B");
    CHECK(session.sessionInfo == "the other room");
    CHECK(session.sessionID != 0);
    CHECK(session.originAddress == "192.168.1.101");
    CHECK(session.sourceAddress == "192.168.1.101");
    CHECK(session.connectionAddress == "239.69.83.172");
    CHECK(session.port == 5006);
    CHECK(session.ttl == 8);
    CHECK(session.encoding == "L16");
    CHECK(session.sampleRate == 44100);
    CHECK(session.numChannels == 2);
    CHECK(session.payloadType == 98);
    CHECK(session.ptimeUs == 250);
    CHECK(session.framecount == 11);
    CHECK(session.ptpDomain == 42);
    CHECK(session.ptpMasterMAC == "00-1D-C1-FF-FE-12-34-56");
    CHECK(session.isValid());
}

TEST_CASE("An encoding with no SDP name of its own is written as L24") {
    StreamInfo info;
    info.name = "Studio B";
    info.multicast = NetworkAddress{"239.69.0.1", 5004};

    info.encoding = AudioEncoding::L24;
    CHECK(SDPParser::fromStreamInfo(info).encoding == "L24");

    info.encoding = AudioEncoding::L16;
    CHECK(SDPParser::fromStreamInfo(info).encoding == "L16");

    // DoP is carried as 24-bit PCM on the wire, and so is Unknown: the switch
    // has no other name to give them.
    info.encoding = AudioEncoding::DoP;
    CHECK(SDPParser::fromStreamInfo(info).encoding == "L24");
    info.encoding = AudioEncoding::Unknown;
    CHECK(SDPParser::fromStreamInfo(info).encoding == "L24");
}

TEST_CASE("A description of our own multicast stream is given a source") {
    SDPSession session;
    session.sessionName = "Studio A";
    session.originAddress = "192.168.1.100";
    session.connectionAddress = "239.69.83.171";

    SDPParser::nameOwnSource(session);
    CHECK(session.sourceAddress == "192.168.1.100");
}

TEST_CASE("nameOwnSource leaves alone every description that is not ours to name") {
    SDPSession session;
    session.sessionName = "Studio A";
    session.originAddress = "192.168.1.100";
    session.connectionAddress = "239.69.83.171";

    SUBCASE("one that already names a source") {
        session.sourceAddress = "10.0.0.5";
        SDPParser::nameOwnSource(session);
        CHECK(session.sourceAddress == "10.0.0.5");
    }
    SUBCASE("one with no origin address to offer") {
        session.originAddress.clear();
        SDPParser::nameOwnSource(session);
        CHECK(session.sourceAddress.empty());
    }
    SUBCASE("a unicast destination, which needs no source filter") {
        session.connectionAddress = "192.168.1.50";
        SDPParser::nameOwnSource(session);
        CHECK(session.sourceAddress.empty());
    }
    SUBCASE("a destination above the multicast range") {
        session.connectionAddress = "240.0.0.1";
        SDPParser::nameOwnSource(session);
        CHECK(session.sourceAddress.empty());
    }
    SUBCASE("a destination with no dot in it at all") {
        session.connectionAddress = "localhost";
        SDPParser::nameOwnSource(session);
        CHECK(session.sourceAddress.empty());
    }
    SUBCASE("a destination whose first octet is not a number") {
        session.connectionAddress = "example.com";
        SDPParser::nameOwnSource(session);
        CHECK(session.sourceAddress.empty());
    }
    SUBCASE("a destination that begins with a dot") {
        session.connectionAddress = ".69.83.171";
        SDPParser::nameOwnSource(session);
        CHECK(session.sourceAddress.empty());
    }
    SUBCASE("an empty destination") {
        session.connectionAddress.clear();
        SDPParser::nameOwnSource(session);
        CHECK(session.sourceAddress.empty());
    }
}

TEST_CASE("Both ends of the multicast range are named, and nothing below it") {
    SDPSession session;
    session.originAddress = "192.168.1.100";

    session.connectionAddress = "224.0.0.1";
    session.sourceAddress.clear();
    SDPParser::nameOwnSource(session);
    CHECK(session.sourceAddress == "192.168.1.100");

    session.connectionAddress = "239.255.255.255";
    session.sourceAddress.clear();
    SDPParser::nameOwnSource(session);
    CHECK(session.sourceAddress == "192.168.1.100");

    session.connectionAddress = "223.255.255.255";
    session.sourceAddress.clear();
    SDPParser::nameOwnSource(session);
    CHECK(session.sourceAddress.empty());
}

TEST_CASE("Blank lines, comments and lines that are not records are skipped") {
    const std::string sdp =
        "v=0\r\n"
        "\r\n"
        "# a comment, which is not SDP but is written into fixtures\r\n"
        "s=Studio A\r\n"
        "x\r\n"                     // too short to be a record
        "not-a-record\r\n"          // no '=' in the second position
        "c=IN IP4 239.69.83.171/32\r\n"
        "m=audio 5004 RTP/AVP 96\r\n"
        "a=rtpmap:96 L24/48000/8\r\n";

    const auto parsed = SDPParser::parseString(sdp);
    REQUIRE(parsed.has_value());
    CHECK(parsed->sessionName == "Studio A");
    CHECK(parsed->numChannels == 8);
}

TEST_CASE("A version this parser does not speak is refused outright") {
    const std::string sdp =
        "v=1\r\n"
        "s=Studio A\r\n"
        "c=IN IP4 239.69.83.171/32\r\n"
        "m=audio 5004 RTP/AVP 96\r\n";

    CHECK_FALSE(SDPParser::parseString(sdp).has_value());
}

TEST_CASE("A malformed record fails the whole description") {
    const std::string head = "v=0\r\ns=Studio A\r\n";
    const std::string tail = "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 L24/48000/8\r\n";

    SUBCASE("an origin line with too few fields") {
        CHECK_FALSE(SDPParser::parseString(head + "o=- 1 0 IN IP4\r\n" + tail).has_value());
    }
    SUBCASE("an origin line whose session id is not a number") {
        CHECK_FALSE(SDPParser::parseString(
            head + "o=- nope 0 IN IP4 192.168.1.100\r\n" + tail).has_value());
    }
    SUBCASE("a connection line with too few fields") {
        CHECK_FALSE(SDPParser::parseString(head + "c=IN IP4\r\n" + tail).has_value());
    }
    SUBCASE("a timing line with one field") {
        CHECK_FALSE(SDPParser::parseString(
            head + "c=IN IP4 239.69.83.171\r\nt=0\r\n" + tail).has_value());
    }
    SUBCASE("a timing line that is not numeric") {
        CHECK_FALSE(SDPParser::parseString(
            head + "c=IN IP4 239.69.83.171\r\nt=now later\r\n" + tail).has_value());
    }
    SUBCASE("a media line with too few fields") {
        CHECK_FALSE(SDPParser::parseString(
            head + "c=IN IP4 239.69.83.171\r\nm=audio 5004 RTP/AVP\r\n").has_value());
    }
    SUBCASE("a media line whose port is not a number") {
        CHECK_FALSE(SDPParser::parseString(
            head + "c=IN IP4 239.69.83.171\r\nm=audio http RTP/AVP 96\r\n").has_value());
    }
    SUBCASE("a media line whose payload type is not a number") {
        CHECK_FALSE(SDPParser::parseString(
            head + "c=IN IP4 239.69.83.171\r\nm=audio 5004 RTP/AVP audio\r\n").has_value());
    }
    SUBCASE("an rtpmap with no format at all") {
        CHECK_FALSE(SDPParser::parseString(
            head + "c=IN IP4 239.69.83.171\r\n" + "a=rtpmap:96\r\n").has_value());
    }
    SUBCASE("an rtpmap whose clock rate is missing") {
        CHECK_FALSE(SDPParser::parseString(
            head + "c=IN IP4 239.69.83.171\r\n" + "a=rtpmap:96 L24\r\n").has_value());
    }
    SUBCASE("an rtpmap whose clock rate is not a number") {
        CHECK_FALSE(SDPParser::parseString(
            head + "c=IN IP4 239.69.83.171\r\n" + "a=rtpmap:96 L24/fast/8\r\n").has_value());
    }
}

TEST_CASE("An rtpmap for a format the m= line did not select is ignored") {
    // A multi-format offer is legal, and every a=rtpmap line used to be taken:
    // the payload type on the line was split out and dropped, so the LAST line
    // won whatever the m= line had selected. The session below is PT 96, 24-bit
    // stereo; taking the PT 97 line left it claiming L16 and 8 channels, and a
    // receiver decoding the real PT 96 traffic at the wrong width and stride
    // produces noise with nothing reported.
    const auto session = SDPParser::parseString(
        "v=0\r\n"
        "o=- 1 0 IN IP4 192.168.1.100\r\n"
        "s=Studio A\r\n"
        "c=IN IP4 239.69.83.171\r\n"
        "t=0 0\r\n"
        "m=audio 5004 RTP/AVP 96\r\n"
        "a=rtpmap:96 L24/48000/2\r\n"
        "a=rtpmap:97 L16/48000/8\r\n");
    REQUIRE(session.has_value());
    CHECK(session->payloadType == 96);
    CHECK(session->encoding == "L24");
    CHECK(session->numChannels == 2);
}

TEST_CASE("A media line whose numbers do not fit their fields is refused") {
    // Both narrow -- port into uint16_t, payloadType into uint8_t -- and an
    // unchecked assignment reduced them modulo the width instead: port 70000
    // became 4464 and the driver bound a port nobody had asked for.
    const std::string head = "v=0\r\ns=Studio A\r\nc=IN IP4 239.69.83.171\r\n";
    const std::string rtpmap = "a=rtpmap:96 L24/48000/2\r\n";
    CHECK_FALSE(SDPParser::parseString(
        head + "m=audio 70000 RTP/AVP 96\r\n" + rtpmap).has_value());
    CHECK_FALSE(SDPParser::parseString(
        head + "m=audio 65536 RTP/AVP 96\r\n" + rtpmap).has_value());
    CHECK_FALSE(SDPParser::parseString(
        head + "m=audio 5004 RTP/AVP 353\r\n" + rtpmap).has_value());
    // A zero channel count is not a stream anything here can carry, and it
    // reached the Manager app as a UInt16 underflow while rendering it.
    CHECK_FALSE(SDPParser::parseString(
        head + "m=audio 5004 RTP/AVP 96\r\n" + "a=rtpmap:96 L24/48000/0\r\n").has_value());
}

TEST_CASE("A description that parses but describes nothing usable is refused") {
    // Every record is well formed; what is missing is the session name, which
    // validation requires.
    const std::string sdp =
        "v=0\r\n"
        "c=IN IP4 239.69.83.171/32\r\n"
        "m=audio 5004 RTP/AVP 96\r\n"
        "a=rtpmap:96 L24/48000/8\r\n";

    CHECK_FALSE(SDPParser::parseString(sdp).has_value());
}

TEST_CASE("A connection address without a TTL keeps the default one") {
    const std::string sdp =
        "v=0\r\n"
        "s=Studio A\r\n"
        "c=IN IP4 239.69.83.171\r\n"
        "m=audio 5004 RTP/AVP 96\r\n"
        "a=rtpmap:96 L24/48000/8\r\n";

    const auto parsed = SDPParser::parseString(sdp);
    REQUIRE(parsed.has_value());
    CHECK(parsed->connectionAddress == "239.69.83.171");
    CHECK(parsed->ttl == 32);
}

TEST_CASE("A TTL that is not a number falls back to 32 rather than failing") {
    const std::string sdp =
        "v=0\r\n"
        "s=Studio A\r\n"
        "c=IN IP4 239.69.83.171/none\r\n"
        "m=audio 5004 RTP/AVP 96\r\n"
        "a=rtpmap:96 L24/48000/8\r\n";

    const auto parsed = SDPParser::parseString(sdp);
    REQUIRE(parsed.has_value());
    CHECK(parsed->connectionAddress == "239.69.83.171");
    CHECK(parsed->ttl == 32);
}

} // namespace Tests
} // namespace AES67
