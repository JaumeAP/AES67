//
// TestPTPLoopback.cpp
// AES67 macOS Driver
//
// The first end-to-end exercise of the full PTP exchange this repo has
// ever run: a real PTPMaster and a real PTPSlave on the same host, over
// loopback multicast, on unprivileged high ports (the §13.1 ports 319/320
// need root, which is why this path had gone unexercised — and why the
// handleDelayResp self-deadlock fixed on 2026-08-31 sat latent).
//
// What this proves, concretely:
//   - the master's Announce/Sync/Follow_Up are accepted by our own slave
//     (wire-format round trip through real sockets, not builders vs parsers);
//   - the master answers Delay_Req with a Delay_Resp the slave accepts —
//     the master half added 2026-08-31 (it never listened on the event
//     port before), and the slave half that used to self-deadlock;
//   - the slave computes offset and mean path delay and reaches lock.
//
// Both clocks read the same host time source, so the true offset is ~0;
// the bounds below are deliberately loose (scheduling noise on a busy
// machine), because the point is that the exchange COMPLETES, not that
// loopback timing is audiophile-grade.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/PTP/PTPMaster.h"
#include "NetworkEngine/PTP/PTPSlave.h"
#include "NetworkEngine/PTP/PTPClockSource.h"

#include <chrono>
#include <iostream>
#include <thread>

using namespace AES67;

namespace {
constexpr uint16_t kTestEventPort = 10319;
constexpr uint16_t kTestGeneralPort = 10320;
constexpr const char* kInterface = "lo0";

template <typename Predicate>
bool waitFor(Predicate&& done, std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return done();
}
} // namespace

TEST_CASE("Master And Slave Complete The Full Exchange Over Loopback") {
    std::cout << "Test: full PTP exchange (Announce/Sync/Follow_Up/"
                 "Delay_Req/Delay_Resp) over loopback... ";

    InternalClockSource clock;

    PTPMasterConfig masterConfig;
    masterConfig.interfaceName = kInterface;
    masterConfig.eventPort = kTestEventPort;
    masterConfig.generalPort = kTestGeneralPort;
    masterConfig.announceIntervalMs = 250; // shrink the listen window
    masterConfig.syncIntervalMs = 125;
    masterConfig.priority1 = 1; // win BMCA outright

    PTPSlaveConfig slaveConfig;
    slaveConfig.interfaceName = kInterface;
    slaveConfig.eventPort = kTestEventPort;
    slaveConfig.generalPort = kTestGeneralPort;
    slaveConfig.announceIntervalMs = 250;
    slaveConfig.delayReqIntervalMs = 250;
    // Both ends live on this host: without loopback the kernel never
    // delivers the slave's Delay_Req to the master's socket.
    slaveConfig.multicastLoopback = true;

    PTPMaster master(masterConfig, clock);
    PTPSlave slave(slaveConfig);

    REQUIRE(master.start());
    REQUIRE(slave.start());

    // The master needs its listen window (3 × 250 ms) before it claims
    // the Master role and starts transmitting; then the slave needs a
    // few Sync/Delay_Req rounds. 15 s is a generous ceiling, and the
    // waits return as soon as the condition holds.
    const bool masterActive = waitFor(
        [&] { return master.isActive(); }, std::chrono::milliseconds(5000));
    CHECK(masterActive);

    const bool slaveLocked = waitFor(
        [&] { return slave.isLocked(); }, std::chrono::milliseconds(15000));
    CHECK(slaveLocked);

    // The delay exchange must have completed both ways: the master
    // answered at least one Delay_Req...
    const bool respSent = waitFor(
        [&] { return master.delayRespSentCount() > 0; },
        std::chrono::milliseconds(5000));
    CHECK(respSent);

    // ...and the slave accepted the answer and derived a path delay.
    // With the pre-2026-08-31 code this is where nothing ever arrives:
    // the first Delay_Resp self-deadlocked the slave's receive thread.
    const bool delayMeasured = waitFor(
        [&] { return slave.getMeanPathDelayNs() != 0; },
        std::chrono::milliseconds(5000));
    CHECK(delayMeasured);

    // Same host, same clock source: offset and path delay are noise-sized.
    // 50 ms bounds tolerate a heavily loaded machine while still catching
    // unit mistakes (a misparsed timestamp is off by seconds, not ms).
    const int64_t offsetNs = slave.getOffsetNs();
    const int64_t delayNs = slave.getMeanPathDelayNs();
    CHECK(std::abs(offsetNs) < 50'000'000);
    CHECK(delayNs > INT64_MIN); // recorded at all
    CHECK(std::abs(delayNs) < 50'000'000);

    std::cout << "offset=" << offsetNs << "ns pathDelay=" << delayNs
              << "ns delayRespSent=" << master.delayRespSentCount() << " ";

    // Teardown must come back — with the old deadlock, slave.stop()'s
    // join() hung forever after the first Delay_Resp.
    slave.stop();
    master.stop();
    CHECK(!slave.isRunning());
    CHECK(!master.isRunning());

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Slave Follows The Intervals The Master Advertises") {
    // IEEE 1588-2008 sec 7.7.2.4 and 9.5.11.2: the rates are the master's to
    // announce. Before this, the master hard-coded 2^0 = 1 s into every
    // logMessageInterval while sending Sync eight times a second, and the
    // slave ignored the field and used its own configuration -- so two
    // implementations that both "worked" only agreed by luck.
    std::cout << "Test: advertised intervals are followed... ";

    InternalClockSource clock;

    PTPMasterConfig masterConfig;
    masterConfig.interfaceName = kInterface;
    masterConfig.eventPort = kTestEventPort + 2;
    masterConfig.generalPort = kTestGeneralPort + 2;
    masterConfig.announceIntervalMs = 250;      // 2^-2 s
    masterConfig.syncIntervalMs = 125;          // 2^-3 s
    masterConfig.logMinDelayReqInterval = -2;   // ask for one every 250 ms
    masterConfig.priority1 = 1;

    PTPSlaveConfig slaveConfig;
    slaveConfig.interfaceName = kInterface;
    slaveConfig.eventPort = kTestEventPort + 2;
    slaveConfig.generalPort = kTestGeneralPort + 2;
    slaveConfig.announceIntervalMs = 1000;      // deliberately wrong
    slaveConfig.delayReqIntervalMs = 4000;      // deliberately wrong
    slaveConfig.multicastLoopback = true;

    PTPMaster master(masterConfig, clock);
    PTPSlave slave(slaveConfig);

    REQUIRE(master.start());
    REQUIRE(slave.start());

    CHECK(waitFor([&] { return master.isActive(); },
                  std::chrono::milliseconds(5000)));

    CHECK(waitFor([&] { return slave.getAdvertisedSyncIntervalMs() == 125; },
                  std::chrono::milliseconds(5000)));
    CHECK(waitFor([&] { return slave.getAdvertisedAnnounceIntervalMs() == 250; },
                  std::chrono::milliseconds(5000)));
    // The Delay_Req rate only arrives with the first Delay_Resp, which the
    // slave has to have asked for first -- at its own (wrong) 4 s to begin
    // with, so this is the slowest of the three to settle.
    CHECK(waitFor([&] { return slave.getAdvertisedDelayReqIntervalMs() == 250; },
                  std::chrono::milliseconds(15000)));

    std::cout << "sync=" << slave.getAdvertisedSyncIntervalMs()
              << "ms announce=" << slave.getAdvertisedAnnounceIntervalMs()
              << "ms delayReq=" << slave.getAdvertisedDelayReqIntervalMs()
              << "ms ";

    slave.stop();
    master.stop();
    std::cout << "PASS" << std::endl;
}

TEST_CASE("Unusable And Foreign-Profile Values Are Refused") {
    PTPSlaveConfig config;
    PTPSlave slave(config);

    // 0x7F means "not being sent" (sec 7.7.2.1), and anything outside the
    // configured bounds is refused rather than obeyed.
    CHECK(slave.logIntervalToMs(0x7F) == 0);
    CHECK(slave.logIntervalToMs(-12) == 0);
    CHECK(slave.logIntervalToMs(12) == 0);
    CHECK(slave.logIntervalToMs(0) == 1000);
    CHECK(slave.logIntervalToMs(-3) == 125);
    CHECK(slave.logIntervalToMs(1) == 2000);
    CHECK(slave.logIntervalToMs(-5) == 31);     // the configured floor

    // majorSdoId is the top nibble of octet 0: 0 here, 1 for 802.1AS.
    PTPHeader header{};
    header.transportAndType = 0x00;             // default profile, Sync
    CHECK(header.getMajorSdoId() == 0);
    header.transportAndType = 0x10;             // gPTP, Sync
    CHECK(header.getMajorSdoId() == 1);
    CHECK(header.getMessageType() == PTPMessageType::Sync);
}
