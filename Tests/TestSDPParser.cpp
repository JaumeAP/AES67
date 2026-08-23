//
// TestSDPParser.cpp
// AES67 macOS Driver - Build #4
// Unit tests for SDP Parser
//

#include "../Driver/SDPParser.h"
#include <iostream>
#include <fstream>
#include <cassert>
#include <sstream>

namespace AES67 {
namespace Tests {

void testBasicSDPParsing() {
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
    assert(session.has_value());
    assert(session->sessionName == "Test Stream");
    assert(session->sessionInfo == "8 Channel Test");
    assert(session->connectionAddress == "239.69.83.171");
    assert(session->port == 5004);
    assert(session->sampleRate == 48000);
    assert(session->numChannels == 8);
    assert(session->encoding == "L24");
    assert(session->ptimeUs == 1000);
    assert(session->framecount == 48);

    std::cout << "✓ PASSED\n";
}

void testRiedelCompatibleSDP() {
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
    assert(session.has_value());
    assert(session->sessionName == "Riedel Artist IFB");
    assert(session->ptpDomain == 0);
    assert(session->ptpMasterMAC == "00-1B-21-AC-B5-4F");
    assert(session->sourceAddress == "192.168.1.100");

    std::cout << "✓ PASSED\n";
}

void testL16Encoding() {
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
    assert(session.has_value());
    assert(session->encoding == "L16");
    assert(session->numChannels == 2);

    std::cout << "✓ PASSED\n";
}

void testHighSampleRates() {
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
    assert(session96.has_value());
    assert(session96->sampleRate == 96000);
    // a=ptime:0.5 — sub-millisecond, and legal SDP. This used to parse to
    // zero: ptime was held as integer milliseconds and read with stoul,
    // which stops at the decimal point. The test file has carried these
    // fractional values since before that was noticed, but only ever
    // asserted the sample rate.
    assert(session96->ptimeUs == 500);

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
    assert(session192.has_value());
    assert(session192->sampleRate == 192000);
    assert(session192->ptimeUs == 250);

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
    assert(session125.has_value());
    assert(session125->ptimeUs == 125);

    // Round trip: a fractional packet time must survive being written back
    // out as SDP, not be rounded to "0" or "1".
    std::string regenerated = SDPParser::generate(*session125);
    assert(regenerated.find("a=ptime:0.125") != std::string::npos);
    auto reparsed = SDPParser::parseString(regenerated);
    assert(reparsed.has_value());
    assert(reparsed->ptimeUs == 125);

    // A whole millisecond must still be written the plain way every other
    // implementation writes it, not as "1.000".
    auto whole = SDPParser::parseString(sdp96);
    assert(whole.has_value());
    whole->ptimeUs = 1000;
    assert(SDPParser::generate(*whole).find("a=ptime:1\n") != std::string::npos);

    std::cout << "✓ PASSED\n";
}

void testMultiChannelConfigurations() {
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
    assert(session64.has_value());
    assert(session64->numChannels == 64);

    std::cout << "✓ PASSED\n";
}

void testSDPGeneration() {
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
    assert(!generated.empty());

    // Verify it can be parsed back
    auto reparsed = SDPParser::parseString(generated);
    assert(reparsed.has_value());
    assert(reparsed->sessionName == session.sessionName);
    assert(reparsed->connectionAddress == session.connectionAddress);
    assert(reparsed->port == session.port);

    std::cout << "✓ PASSED\n";
}

void testInvalidSDP() {
    std::cout << "Test: Invalid SDP Handling... ";

    // Empty SDP
    auto empty = SDPParser::parseString("");
    assert(!empty.has_value());

    // Missing required fields
    std::string incomplete = R"(v=0
s=Incomplete
)";
    auto inc = SDPParser::parseString(incomplete);
    assert(!inc.has_value());

    std::cout << "✓ PASSED\n";
}

void testFileOperations() {
    std::cout << "Test: File Operations... ";

    // Create test SDP file
    std::string testPath = "/tmp/test_aes67.sdp";
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
    assert(session.has_value());
    assert(session->sessionName == "File Test");

    // Cleanup
    std::remove(testPath.c_str());

    std::cout << "✓ PASSED\n";
}

void runAllTests() {
    std::cout << "\n=== AES67 SDP Parser Test Suite ===\n\n";

    testBasicSDPParsing();
    testRiedelCompatibleSDP();
    testL16Encoding();
    testHighSampleRates();
    testMultiChannelConfigurations();
    testSDPGeneration();
    testInvalidSDP();
    testFileOperations();

    std::cout << "\n✅ All SDP Parser tests passed!\n\n";
}

} // namespace Tests
} // namespace AES67

int main() {
    try {
        AES67::Tests::runAllTests();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "❌ Test failed with exception: " << e.what() << "\n";
        return 1;
    }
}
