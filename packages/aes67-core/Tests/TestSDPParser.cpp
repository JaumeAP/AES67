//
// TestSDPParser.cpp
// AES67 macOS Driver - Build #4
// Unit tests for SDP Parser
//

#include "Driver/SDPParser.h"
#include <iostream>
#include <fstream>
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <string>
#include <sstream>

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
    CHECK(SDPParser::generate(*whole).find("a=ptime:1\n") != std::string::npos);;

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
    CHECK(generated.find(" IN IP4 192.168.1.200\n") != std::string::npos);
    CHECK(generated.find("IP4 IN") == std::string::npos);

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
    CHECK(gen.find("a=ts-refclk:ptp=IEEE1588-2008:00-60-2B-FF-FE-11-22-33:0\n") != std::string::npos);
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
