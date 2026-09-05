//
// TestRTCPMonitor.cpp
// AES67 macOS Driver
//
// What the monitor does with an RTCP packet once it has one: which reporters
// it counts, what it records about them, and what it declines to count.
//
// The parsing itself belongs to RTCPReceiverTable and is tested in the core.
// What was untested until 2026-09-04 is everything between the socket and the
// table — the SSRC-to-CNAME pairing, the source address, and whether a packet
// that says nothing useful still creates a reporter out of nothing.
//

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/Discovery/RTCPMonitor.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using namespace AES67;

namespace {

void appendBE32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

/// A minimal Receiver Report: version 2, no report blocks, just the reporting
/// SSRC. Eight bytes, which is what an AES67 receiver sends when it has
/// nothing to complain about.
std::vector<uint8_t> receiverReport(uint32_t ssrc) {
    std::vector<uint8_t> packet;
    packet.push_back(0x80);  // V=2, P=0, RC=0
    packet.push_back(201);   // PT = RR
    packet.push_back(0x00);
    packet.push_back(0x01);  // length: 2 words after the first
    appendBE32(packet, ssrc);
    return packet;
}

/// An SDES packet carrying one CNAME for `ssrc`, padded to a 32-bit boundary
/// as RFC 3550 §6.5 requires.
std::vector<uint8_t> sdesWithCname(uint32_t ssrc, const std::string& cname) {
    std::vector<uint8_t> chunk;
    appendBE32(chunk, ssrc);
    chunk.push_back(1);  // CNAME
    chunk.push_back(static_cast<uint8_t>(cname.size()));
    chunk.insert(chunk.end(), cname.begin(), cname.end());
    chunk.push_back(0);  // end of the item list
    while (chunk.size() % 4 != 0) chunk.push_back(0);

    std::vector<uint8_t> packet;
    packet.push_back(0x81);  // V=2, SC=1
    packet.push_back(202);   // PT = SDES
    const size_t words = chunk.size() / 4;  // header word is not counted
    packet.push_back(static_cast<uint8_t>((words >> 8) & 0xFF));
    packet.push_back(static_cast<uint8_t>(words & 0xFF));
    packet.insert(packet.end(), chunk.begin(), chunk.end());
    return packet;
}

void deliver(RTCPMonitor& monitor, const std::vector<uint8_t>& packet,
             const std::string& from) {
    monitor.deliverPacket(packet.data(), packet.size(), from);
}

const RTCPReporter* find(const std::vector<RTCPReporter>& reporters, uint32_t ssrc) {
    const auto it = std::find_if(reporters.begin(), reporters.end(),
                                 [ssrc](const RTCPReporter& r) { return r.ssrc == ssrc; });
    return it == reporters.end() ? nullptr : &*it;
}

} // namespace

TEST_CASE("A receiver report makes a reporter") {
    RTCPMonitor monitor;
    deliver(monitor, receiverReport(0x11112222), "192.168.1.60");

    const auto reporters = monitor.reporters();
    REQUIRE(reporters.size() == 1);
    CHECK(reporters[0].ssrc == 0x11112222);
    CHECK(reporters[0].sourceIp == "192.168.1.60");
}

TEST_CASE("Two hosts reporting are two receivers, one host twice is one") {
    // Counting distinct receivers of what this driver sends is the whole
    // point of the class, so the arithmetic is worth pinning.
    RTCPMonitor monitor;
    deliver(monitor, receiverReport(0xAAAA0001), "192.168.1.61");
    deliver(monitor, receiverReport(0xBBBB0002), "192.168.1.62");
    CHECK(monitor.reporters().size() == 2);

    deliver(monitor, receiverReport(0xAAAA0001), "192.168.1.61");
    CHECK(monitor.reporters().size() == 2);
}

TEST_CASE("A CNAME is attached to the SSRC that owns it") {
    // A compound packet is the usual shape: the report and the SDES that
    // names its sender, in one datagram.
    RTCPMonitor monitor;
    std::vector<uint8_t> compound = receiverReport(0xC0DE0001);
    const std::vector<uint8_t> sdes = sdesWithCname(0xC0DE0001, "dac3202@192.168.1.63");
    compound.insert(compound.end(), sdes.begin(), sdes.end());

    deliver(monitor, compound, "192.168.1.63");

    const auto reporters = monitor.reporters();
    REQUIRE(reporters.size() == 1);
    CHECK(reporters[0].cname == "dac3202@192.168.1.63");
}

TEST_CASE("A CNAME belonging to somebody else is not borrowed") {
    RTCPMonitor monitor;
    std::vector<uint8_t> compound = receiverReport(0x00000101);
    const std::vector<uint8_t> sdes = sdesWithCname(0x00000202, "someone-else");
    compound.insert(compound.end(), sdes.begin(), sdes.end());

    deliver(monitor, compound, "192.168.1.64");

    const auto reporters = monitor.reporters();
    const RTCPReporter* reporter = find(reporters, 0x00000101);
    REQUIRE(reporter != nullptr);
    CHECK(reporter->cname.empty());
}

TEST_CASE("Nothing usable makes no reporter") {
    RTCPMonitor monitor;

    // Too short to be an RTCP packet at all.
    const std::vector<uint8_t> runt = {0x80, 201};
    deliver(monitor, runt, "192.168.1.65");
    CHECK(monitor.reporters().empty());

    // RTCP version 1, which this does not speak.
    std::vector<uint8_t> wrongVersion = receiverReport(0xDEAD0001);
    wrongVersion[0] = 0x40;
    deliver(monitor, wrongVersion, "192.168.1.65");
    CHECK(monitor.reporters().empty());

    // A length field that claims more than the datagram carries.
    std::vector<uint8_t> truncated = receiverReport(0xDEAD0002);
    truncated[3] = 0x40;
    deliver(monitor, truncated, "192.168.1.65");
    CHECK(monitor.reporters().empty());

    // An APP packet: well formed, and nothing this counts.
    std::vector<uint8_t> app = receiverReport(0xDEAD0003);
    app[1] = 204;  // PT = APP
    deliver(monitor, app, "192.168.1.65");
    CHECK(monitor.reporters().empty());
}

TEST_CASE("The address recorded is the sender's, not the report's") {
    // An SSRC is a 32-bit number its owner picks; the address is what the
    // packet came from. Confusing the two would let one host be counted as
    // several, or several as one.
    RTCPMonitor monitor;
    deliver(monitor, receiverReport(0x50505050), "10.0.0.7");

    const auto reporters = monitor.reporters();
    REQUIRE(reporters.size() == 1);
    CHECK(reporters[0].sourceIp == "10.0.0.7");
}
