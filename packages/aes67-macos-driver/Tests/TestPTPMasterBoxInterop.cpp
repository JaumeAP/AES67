//
// TestPTPMasterBoxInterop.cpp
// AES67 macOS Driver
//
// PTPSlave against the traffic the AES67-MasterBox actually puts on the
// wire.
//
// The other PTP suites check this slave against itself: TestPTPLoopback
// pairs it with our own PTPMaster, which agrees with it by construction.
// That says nothing about the grandmaster this driver is meant to follow,
// a Teensy 4.1 running JaumeAP/t41-ptp on the `integration/master-box`
// branch, and until this file existed the two had only ever been compared
// by reading their source side by side.
//
// So the messages here are not invented: every byte is written where
// `lib/t41-ptp/src/ptp/ptp-base.cpp` writes it -- `initPTPMessage` for the
// common header, `announceMessage`, `syncMessage`, `followUpMessage` and
// `delayResponseMessage` for the rest -- with the values the box's default
// profile announces (`src/profiles.cpp`: domain 0, 8 sync per second,
// announce every second, priority1 and priority2 128) and the clockClass it
// swings between (`src/main.cpp`: 248 free-running, 13 once the word clock
// has it locked).
//
// It is a replay, not a capture. A capture off the real box would be worth
// more and would go in the same shape: bytes in through deliverMessage(),
// state out through the getters.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/PTP/PTPDiagnostics.h"
#include "NetworkEngine/PTP/PTPSlave.h"

#include <array>
#include <cstdint>
#include <vector>

using namespace AES67;

namespace {

// The box's clock identity, an EUI-64 built from the Teensy's MAC the way
// the library does it.
constexpr std::array<uint8_t, 8> kBoxClockId{0x04, 0xE9, 0xE5, 0xFF,
                                             0xFE, 0x12, 0x34, 0x56};
// A second grandmaster, for the comparisons that need two.
constexpr std::array<uint8_t, 8> kOtherClockId{0x00, 0x1D, 0xC1, 0xFF,
                                               0xFE, 0xAA, 0xBB, 0xCC};

constexpr uint8_t kClockClassLocked = 13;    // src/main.cpp
constexpr uint8_t kClockClassFree = 248;
constexpr uint8_t kAccuracyUnknown = 0xFE;   // what the box leaves it at
constexpr uint16_t kVarianceUnknown = 0xFFFF;
constexpr uint8_t kTimeSourceInternal = 0xA0;

/// The common header, exactly as PTPBase::initPTPMessage writes it.
std::vector<uint8_t> commonHeader(uint8_t messageType, uint16_t size, uint8_t control,
                                  uint16_t sequenceId,
                                  const std::array<uint8_t, 8>& clockId,
                                  uint8_t domain = 0, uint8_t majorSdoId = 0) {
    std::vector<uint8_t> buf(size, 0);
    buf[0] = static_cast<uint8_t>((majorSdoId << 4) | messageType);
    buf[1] = 2;                                        // versionPTP
    buf[2] = static_cast<uint8_t>((size >> 8) & 0xFF);
    buf[3] = static_cast<uint8_t>(size & 0xFF);
    buf[4] = domain;
    for (size_t i = 0; i < clockId.size(); ++i) buf[20 + i] = clockId[i];
    buf[28] = 0;                                       // portNumber 1
    buf[29] = 1;
    buf[30] = static_cast<uint8_t>((sequenceId >> 8) & 0xFF);
    buf[31] = static_cast<uint8_t>(sequenceId & 0xFF);
    buf[32] = control;
    buf[33] = 0x7F;                                    // overwritten per message
    return buf;
}

/// PTPBase::timespecToBuffer: six bytes of seconds, four of nanoseconds,
/// from byte 34.
void writeTimestamp(std::vector<uint8_t>& buf, uint64_t nanoseconds) {
    const uint64_t seconds = nanoseconds / 1000000000ULL;
    const uint32_t nanos = static_cast<uint32_t>(nanoseconds % 1000000000ULL);
    buf[34] = static_cast<uint8_t>((seconds >> 40) & 0xFF);
    buf[35] = static_cast<uint8_t>((seconds >> 32) & 0xFF);
    buf[36] = static_cast<uint8_t>((seconds >> 24) & 0xFF);
    buf[37] = static_cast<uint8_t>((seconds >> 16) & 0xFF);
    buf[38] = static_cast<uint8_t>((seconds >> 8) & 0xFF);
    buf[39] = static_cast<uint8_t>(seconds & 0xFF);
    buf[40] = static_cast<uint8_t>((nanos >> 24) & 0xFF);
    buf[41] = static_cast<uint8_t>((nanos >> 16) & 0xFF);
    buf[42] = static_cast<uint8_t>((nanos >> 8) & 0xFF);
    buf[43] = static_cast<uint8_t>(nanos & 0xFF);
}

/// PTPBase::announceMessage.
std::vector<uint8_t> announce(const std::array<uint8_t, 8>& clockId, uint8_t clockClass,
                              uint8_t priority1 = 128, uint8_t priority2 = 128,
                              int8_t logAnnounceInterval = 0,
                              uint8_t accuracy = kAccuracyUnknown,
                              uint16_t variance = kVarianceUnknown,
                              uint16_t sequenceId = 1, uint8_t majorSdoId = 0) {
    std::vector<uint8_t> buf = commonHeader(11, 64, 5, sequenceId, clockId, 0, majorSdoId);
    // flagField octet 1: PTPTimescale set, currentUtcOffsetValid clear -- the
    // box has frequency and an edge per second but no traceable absolute
    // time, and says so.
    buf[7] = 0x08;
    buf[33] = static_cast<uint8_t>(logAnnounceInterval);
    buf[44] = 0;                                       // currentUtcOffset 37
    buf[45] = 37;
    buf[47] = priority1;
    buf[48] = clockClass;
    buf[49] = accuracy;
    buf[50] = static_cast<uint8_t>((variance >> 8) & 0xFF);
    buf[51] = static_cast<uint8_t>(variance & 0xFF);
    buf[52] = priority2;
    for (size_t i = 0; i < clockId.size(); ++i) buf[53 + i] = clockId[i];
    buf[63] = kTimeSourceInternal;
    return buf;
}

/// PTPBase::syncMessage. Two-step: the flag is set and the timestamp in the
/// Sync itself stays zero, the real t1 arriving in the Follow_Up.
std::vector<uint8_t> sync(const std::array<uint8_t, 8>& clockId, uint16_t sequenceId,
                          int8_t logSyncInterval = -3, bool twoStep = true) {
    std::vector<uint8_t> buf = commonHeader(0, 44, 0, sequenceId, clockId);
    buf[6] = twoStep ? 2 : 0;
    buf[33] = static_cast<uint8_t>(logSyncInterval);
    return buf;
}

/// PTPBase::followUpMessage.
std::vector<uint8_t> followUp(const std::array<uint8_t, 8>& clockId, uint16_t sequenceId,
                              uint64_t t1Ns, int8_t logSyncInterval = -3) {
    std::vector<uint8_t> buf = commonHeader(8, 44, 2, sequenceId, clockId);
    buf[33] = static_cast<uint8_t>(logSyncInterval);
    writeTimestamp(buf, t1Ns);
    return buf;
}

/// PTPBase::delayResponseMessage. logMinDelayReqInterval is 0 there, which
/// is one Delay_Req per second.
std::vector<uint8_t> delayResp(const std::array<uint8_t, 8>& clockId, uint16_t sequenceId,
                               uint64_t t4Ns, const PTPPortIdentity& requesting) {
    std::vector<uint8_t> buf = commonHeader(9, 54, 3, sequenceId, clockId);
    buf[33] = 0;
    writeTimestamp(buf, t4Ns);
    for (int i = 0; i < 8; ++i) buf[44 + i] = requesting.clockIdentity.id[i];
    buf[52] = static_cast<uint8_t>((requesting.portNumber >> 8) & 0xFF);
    buf[53] = static_cast<uint8_t>(requesting.portNumber & 0xFF);
    return buf;
}

void deliverGeneral(PTPSlave& slave, const std::vector<uint8_t>& msg) {
    slave.deliverMessage(msg.data(), msg.size(), 0, false);
}

void deliverEvent(PTPSlave& slave, const std::vector<uint8_t>& msg, uint64_t receiveTimeNs) {
    slave.deliverMessage(msg.data(), msg.size(), receiveTimeNs, true);
}

PTPSlaveConfig boxFacingConfig() {
    PTPSlaveConfig config;
    config.domain = 0;              // the box's every profile is domain 0
    config.interfaceName = "lo0";   // never opened: nothing here calls start()
    return config;
}

PTPDiagnostics diagnosticsOf(const PTPSlave& slave) {
    PTPDiagnostics diag;
    slave.updateDiagnostics(diag);
    return diag;
}

} // namespace

TEST_CASE("The box's Announce is taken, and its interval with it") {
    PTPSlave slave(boxFacingConfig());
    deliverGeneral(slave, announce(kBoxClockId, kClockClassFree));

    const PTPDiagnostics diag = diagnosticsOf(slave);
    CHECK(diag.isConnected);
    CHECK(diag.announceMessagesReceived == 1);
    CHECK(diag.clockClass == kClockClassFree);
    CHECK(diag.clockAccuracy == kAccuracyUnknown);
    CHECK(slave.getClockClass() == kClockClassFree);

    // logAnnounceInterval 0 is one per second, which is what the default
    // profile sends and what the announce timeout is then measured against.
    CHECK(slave.getAdvertisedAnnounceIntervalMs() == 1000);
}

TEST_CASE("A free-running box is followed rather than ignored") {
    // clockClass 248 says "not traceable", which is what the box announces
    // until the word clock has it locked. Refusing it would leave this
    // driver with no reference at all during that window, and the box's
    // free-running oscillator still beats the local clock.
    PTPSlave slave(boxFacingConfig());
    deliverGeneral(slave, announce(kBoxClockId, kClockClassFree));
    CHECK(diagnosticsOf(slave).isConnected);
}

TEST_CASE("Between two boxes, the locked one wins") {
    PTPSlave slave(boxFacingConfig());
    deliverGeneral(slave, announce(kBoxClockId, kClockClassFree));
    deliverGeneral(slave, announce(kOtherClockId, kClockClassLocked));
    CHECK(slave.getClockClass() == kClockClassLocked);

    // And the standby profile stays out of the way: priority1 200 loses to
    // 128 whatever its clockClass says.
    deliverGeneral(slave, announce(kBoxClockId, kClockClassLocked, 200));
    CHECK(slave.getClockClass() == kClockClassLocked);
    CHECK(diagnosticsOf(slave).masterClockID != "");
}

TEST_CASE("Two masters alike but for the accuracy are told apart") {
    // The dataset comparison reaches clockAccuracy before priority2. Only
    // priority1, clockClass and priority2 were compared until 2026-09-03,
    // so this pair used to come out equal and the first one heard kept the
    // port whatever the second one was worth.
    PTPSlave slave(boxFacingConfig());
    deliverGeneral(slave, announce(kBoxClockId, kClockClassLocked, 128, 128, 0,
                                   kAccuracyUnknown));
    deliverGeneral(slave, announce(kOtherClockId, kClockClassLocked, 128, 128, 0,
                                   0x21 /* within 100 ns */));
    CHECK(diagnosticsOf(slave).clockAccuracy == 0x21);
}

TEST_CASE("Sync and Follow_Up carry the offset") {
    PTPSlave slave(boxFacingConfig());
    deliverGeneral(slave, announce(kBoxClockId, kClockClassLocked));

    // One cycle with numbers chosen so the arithmetic is readable: the box
    // sends at t1, this slave's clock reads t2 when it arrives. No Delay_Req
    // has gone out, so the path delay is still zero and the offset comes out
    // as t2 - t1 -- the sum of the real offset and the link delay, which is
    // exactly what a slave knows before it has measured the link.
    const uint64_t t1 = 1000000000ULL;
    const uint64_t t2 = 1001100000ULL;

    deliverEvent(slave, sync(kBoxClockId, 7), t2);
    deliverGeneral(slave, followUp(kBoxClockId, 7, t1));

    const PTPDiagnostics diag = diagnosticsOf(slave);
    CHECK(diag.syncMessagesReceived == 1);
    CHECK(diag.followUpMessagesReceived == 1);
    CHECK(slave.getOffsetNs() == static_cast<int64_t>(t2 - t1));

    // 8 sync per second, which is what the default profile advertises.
    CHECK(slave.getAdvertisedSyncIntervalMs() == 125);
}

TEST_CASE("A Follow_Up for another Sync is not taken") {
    PTPSlave slave(boxFacingConfig());
    deliverEvent(slave, sync(kBoxClockId, 7), 1001100000ULL);
    deliverGeneral(slave, followUp(kBoxClockId, 8, 1000000000ULL));

    // It is counted as received, because it was: the counters say what came
    // off the wire. What matters is that its t1 went nowhere, so no offset
    // was ever computed from a timestamp belonging to another cycle.
    const PTPDiagnostics diag = diagnosticsOf(slave);
    CHECK(diag.syncMessagesReceived == 1);
    CHECK(diag.followUpMessagesReceived == 1);
    CHECK(slave.getOffsetNs() == 0);
}

TEST_CASE("A one-step Sync is refused, and counted") {
    // The box is two-step. `twoStepOnly` says to follow only those, and it
    // was read by nobody until 2026-09-03: a one-step master was taken
    // regardless of the setting.
    PTPSlave slave(boxFacingConfig());
    deliverEvent(slave, sync(kBoxClockId, 7, -3, /*twoStep=*/false), 1001100000ULL);

    CHECK(slave.getOneStepRejectedCount() == 1);
    CHECK(diagnosticsOf(slave).syncMessagesReceived == 0);
}

TEST_CASE("The rate the box asks Delay_Req at comes off its Delay_Resp") {
    PTPSlave slave(boxFacingConfig());
    // Nothing is pending, so the measurement goes nowhere -- but the rate
    // the master is asking of everyone is worth having either way, and
    // logMinDelayReqInterval 0 is one per second.
    deliverGeneral(slave, delayResp(kBoxClockId, 3, 1000200000ULL, slave.getPortIdentity()));
    CHECK(slave.getAdvertisedDelayReqIntervalMs() == 1000);
    CHECK(diagnosticsOf(slave).delayRespMessagesReceived == 1);
}

TEST_CASE("gPTP traffic on the same domain is left alone") {
    // majorSdoId 1 is 802.1AS. Same wire, same domain number, another
    // profile: following it would be following a clock this driver has no
    // business on.
    PTPSlave slave(boxFacingConfig());
    deliverGeneral(slave, announce(kBoxClockId, kClockClassLocked, 128, 128, 0,
                                   kAccuracyUnknown, kVarianceUnknown, 1,
                                   /*majorSdoId=*/1));

    CHECK(slave.getSdoIdMismatchCount() == 1);
    CHECK_FALSE(diagnosticsOf(slave).isConnected);
}

TEST_CASE("Each message is only taken on the port that carries it") {
    // IEEE 1588-2008 Table 15: Sync is an event message and Announce a
    // general one. A Sync arriving on 320 has no usable timestamp behind it,
    // and an Announce on 319 is not this profile's traffic.
    PTPSlave slave(boxFacingConfig());
    deliverEvent(slave, announce(kBoxClockId, kClockClassLocked), 1000000000ULL);
    CHECK_FALSE(diagnosticsOf(slave).isConnected);

    deliverGeneral(slave, sync(kBoxClockId, 7));
    CHECK(diagnosticsOf(slave).syncMessagesReceived == 0);
}
