//
// TestPTPT41Interop.cpp
// AES67 macOS Driver
//
// PTPSlave against the bytes t41-ptp actually emits when it runs.
//
// TestPTPMasterBoxInterop already replays a grandmaster's traffic through
// deliverMessage(), but the bytes there are written by hand from reading
// `packages/t41-ptp/src/ptp/ptp-send.cpp` -- two implementations compared by
// eye, and the comparison goes stale the moment either side changes. This
// suite replays a fixture the library produces by running: the host build of
// t41-ptp in master mode, with its Arduino and QNEthernet stubs, and every
// datagram it tries to send written out verbatim
// (`packages/t41-ptp/test/gen_interop_fixture.cpp`, `make -C test fixture`).
//
// So a change to what the Teensy puts on the wire reaches this suite as a
// changed fixture, and a disagreement between the two implementations fails
// here rather than on a bench.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/PTP/PTPDiagnostics.h"
#include "NetworkEngine/PTP/PTPSlave.h"

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace AES67;

namespace {

/// One line of the fixture: what the library sent, and on which socket.
struct EmittedMessage {
    std::string name;                 // Sync, Follow_Up, Announce, Delay_Resp
    bool onEventSocket{false};        // event is 319, general is 320
    std::vector<uint8_t> bytes;
};

std::vector<EmittedMessage> readFixture(const std::string& path) {
    std::vector<EmittedMessage> messages;
    std::ifstream file(path);
    REQUIRE_MESSAGE(file.is_open(), "fixture not found: " << path);

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream fields(line);
        std::string name, socketName, hex;
        fields >> name >> socketName >> hex;
        if (hex.empty()) continue;

        EmittedMessage message;
        message.name = name;
        message.onEventSocket = (socketName == "event");
        message.bytes.reserve(hex.size() / 2);
        for (size_t i = 0; i + 1 < hex.size(); i += 2) {
            message.bytes.push_back(
                static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
        }
        messages.push_back(std::move(message));
    }
    return messages;
}

std::vector<EmittedMessage> fixture() {
    static const std::vector<EmittedMessage> messages = readFixture(T41_FIXTURE_PATH);
    return messages;
}

std::vector<EmittedMessage> messagesNamed(const std::string& name) {
    std::vector<EmittedMessage> matching;
    for (const EmittedMessage& message : fixture()) {
        if (message.name == name) matching.push_back(message);
    }
    return matching;
}

PTPSlaveConfig t41FacingConfig() {
    PTPSlaveConfig config;
    config.domain = 0;              // what the library's default profile sends
    config.interfaceName = "lo0";   // never opened: nothing here calls start()
    return config;
}

PTPDiagnostics diagnosticsOf(const PTPSlave& slave) {
    PTPDiagnostics diag;
    slave.updateDiagnostics(diag);
    return diag;
}

void deliver(PTPSlave& slave, const EmittedMessage& message, uint64_t receiveTimeNs) {
    slave.deliverMessage(message.bytes.data(), message.bytes.size(), receiveTimeNs,
                         message.onEventSocket);
}

/// The ten-byte requesting port identity a Delay_Resp carries back, at
/// offset 44. The fixture holds a placeholder there because that field
/// belongs to whichever slave asked.
void setRequestingPortIdentity(std::vector<uint8_t>& message, const PTPPortIdentity& identity) {
    REQUIRE(message.size() >= 54);
    for (int i = 0; i < 8; ++i) message[44 + i] = identity.clockIdentity.id[i];
    message[52] = static_cast<uint8_t>((identity.portNumber >> 8) & 0xFF);
    message[53] = static_cast<uint8_t>(identity.portNumber & 0xFF);
}

/// Seconds and nanoseconds from byte 34, the way both implementations write
/// a PTP timestamp.
uint64_t readTimestamp(const std::vector<uint8_t>& message) {
    uint64_t seconds = 0;
    for (int i = 0; i < 6; ++i) {
        seconds = (seconds << 8) | message[34 + static_cast<size_t>(i)];
    }
    uint32_t nanos = 0;
    for (int i = 0; i < 4; ++i) {
        nanos = (nanos << 8) | message[40 + static_cast<size_t>(i)];
    }
    return seconds * 1000000000ULL + nanos;
}

} // namespace

TEST_CASE("The fixture holds the exchange the library emits") {
    // If this fails, the generator changed shape and the rest of the suite is
    // testing something other than what it says.
    CHECK(messagesNamed("Announce").size() == 1);
    CHECK(messagesNamed("Sync").size() == 3);
    CHECK(messagesNamed("Follow_Up").size() == 3);
    CHECK(messagesNamed("Delay_Resp").size() == 1);

    for (const EmittedMessage& message : fixture()) {
        CHECK(message.bytes[1] == 2);           // versionPTP
        CHECK((message.bytes[4] == 0));         // domain 0
    }
    // Sync goes out on the event socket, everything else on the general one.
    for (const EmittedMessage& message : messagesNamed("Sync")) {
        CHECK(message.onEventSocket);
    }
    for (const EmittedMessage& message : messagesNamed("Follow_Up")) {
        CHECK_FALSE(message.onEventSocket);
    }
}

TEST_CASE("The library's Announce is accepted and its clock described") {
    PTPSlave slave(t41FacingConfig());
    deliver(slave, messagesNamed("Announce").front(), 0);

    const PTPDiagnostics diag = diagnosticsOf(slave);
    CHECK(diag.isConnected);
    CHECK(diag.announceMessagesReceived == 1);
    // Whatever the library announces, the slave has to read the same values
    // back out of it rather than assume them.
    const std::vector<uint8_t> announce = messagesNamed("Announce").front().bytes;
    CHECK(diag.clockClass == announce[48]);
    CHECK(diag.clockAccuracy == announce[49]);
}

TEST_CASE("Sync and Follow_Up from the library carry an offset the slave computes") {
    PTPSlave slave(t41FacingConfig());
    deliver(slave, messagesNamed("Announce").front(), 0);

    const std::vector<EmittedMessage> syncs = messagesNamed("Sync");
    const std::vector<EmittedMessage> followUps = messagesNamed("Follow_Up");
    REQUIRE(syncs.size() == followUps.size());

    // A receive time a fixed amount after each departure: the offset the
    // slave reports has to be that amount, since the fixture's t1 is the
    // library's own departure timestamp.
    constexpr uint64_t kApparentOffsetNs = 1500;
    const uint64_t t1 = readTimestamp(followUps.front().bytes);

    deliver(slave, syncs.front(), t1 + kApparentOffsetNs);
    deliver(slave, followUps.front(), 0);

    CHECK(slave.getOffsetNs() == static_cast<int64_t>(kApparentOffsetNs));
    CHECK(diagnosticsOf(slave).syncMessagesReceived == 1);

    // The two-step flag is set in every Sync the library sends, which is why
    // the timestamp that counts is the Follow_Up's and not the Sync's.
    CHECK((syncs.front().bytes[6] & 0x02) != 0);
    CHECK(readTimestamp(syncs.front().bytes) == 0);
}

TEST_CASE("Every Sync in the fixture pairs with its own Follow_Up") {
    PTPSlave slave(t41FacingConfig());
    deliver(slave, messagesNamed("Announce").front(), 0);

    const std::vector<EmittedMessage> syncs = messagesNamed("Sync");
    const std::vector<EmittedMessage> followUps = messagesNamed("Follow_Up");

    for (size_t i = 0; i < syncs.size(); ++i) {
        // Sequence IDs live at bytes 30-31 and have to match, or the slave
        // is pairing timestamps from different exchanges.
        CHECK(syncs[i].bytes[30] == followUps[i].bytes[30]);
        CHECK(syncs[i].bytes[31] == followUps[i].bytes[31]);

        const uint64_t t1 = readTimestamp(followUps[i].bytes);
        deliver(slave, syncs[i], t1 + 2000);
        deliver(slave, followUps[i], 0);
        CHECK(slave.getOffsetNs() == 2000);
    }
    CHECK(diagnosticsOf(slave).syncMessagesReceived == syncs.size());
}

TEST_CASE("The library's Delay_Resp is recognised as ours only by identity") {
    PTPSlave slave(t41FacingConfig());
    deliver(slave, messagesNamed("Announce").front(), 0);

    // As it comes out of the library: addressed to the placeholder requester,
    // which is nobody here.
    const EmittedMessage asEmitted = messagesNamed("Delay_Resp").front();
    deliver(slave, asEmitted, 0);
    const uint64_t strangersRejected = diagnosticsOf(slave).delayRespMessagesReceived;

    // Addressed to this slave, it is taken up.
    EmittedMessage addressed = asEmitted;
    setRequestingPortIdentity(addressed.bytes, slave.getPortIdentity());
    deliver(slave, addressed, 0);

    CHECK(diagnosticsOf(slave).delayRespMessagesReceived > strangersRejected);
}
