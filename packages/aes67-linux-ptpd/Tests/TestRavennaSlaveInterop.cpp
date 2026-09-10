//
// TestRavennaSlaveInterop.cpp
// AES67 Linux PTP daemon
// This grandmaster against the RAVENNA ALSA module's rules for a master.
//
// The module is the slave half of the Linux device this repository vendors
// (packages/aes67-linux-daemon), and it is stricter than IEEE 1588: it elects
// one master and drops everything else, it wants the domain it was configured
// with, and it drops its lock when Sync sequence numbers are not contiguous.
// support/RavennaSlave mirrors those rules from PTP.c; this feeds them the
// bytes PtpWire actually builds.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "PtpWire.h"
#include "support/RavennaSlave.h"

#include "Profiles/PtpProfiles.h"

#include <cstdint>
#include <vector>

using namespace AES67;
using namespace AES67::LinuxPtpd;
using namespace AES67::LinuxPtpd::Tests;

namespace {

PortContext masterPort(uint8_t domain = 0) {
    PortContext port;
    port.clockIdentity.id = {0xB8, 0x27, 0xEB, 0xFF, 0xFE, 0x01, 0x02, 0x03};
    port.portNumber = 1;
    port.domainNumber = domain;
    port.majorSdoId = 0;
    return port;
}

AnnounceDataset dataset() {
    AnnounceDataset set;
    set.clockIdentity.id = {0xB8, 0x27, 0xEB, 0xFF, 0xFE, 0x01, 0x02, 0x03};
    set.clockClass = 248;
    set.timeSource = 0xA0;
    return set;
}

std::vector<uint8_t> announce(const PortContext& port, uint16_t sequenceId,
                              int8_t logAnnounceInterval = 0) {
    std::vector<uint8_t> buffer(kAnnounceSize);
    const size_t written = buildAnnounce(buffer.data(), buffer.size(), port, dataset(),
                                         sequenceId, logAnnounceInterval, 0);
    REQUIRE(written == kAnnounceSize);
    return buffer;
}

std::vector<uint8_t> sync(const PortContext& port, uint16_t sequenceId,
                          int8_t logSyncInterval = -3) {
    std::vector<uint8_t> buffer(kSyncSize);
    const size_t written = buildSync(buffer.data(), buffer.size(), port, sequenceId,
                                     logSyncInterval);
    REQUIRE(written == kSyncSize);
    return buffer;
}

std::vector<uint8_t> followUp(const PortContext& port, uint16_t sequenceId,
                              uint64_t preciseOriginNs) {
    std::vector<uint8_t> buffer(kFollowUpSize);
    const size_t written = buildFollowUp(buffer.data(), buffer.size(), port, sequenceId,
                                         -3, preciseOriginNs);
    REQUIRE(written == kFollowUpSize);
    return buffer;
}

SlaveVerdict give(SlaveState& state, const std::vector<uint8_t>& message,
                  uint16_t port = 320) {
    return feed(state, message.data(), message.size(), port);
}

} // namespace

TEST_CASE("The module elects this grandmaster and follows its Sync") {
    const PortContext port = masterPort();
    SlaveState slave;

    const SlaveVerdict elected = give(slave, announce(port, 1));
    INFO("refused because: ", elected.reason);
    REQUIRE(elected.used);
    CHECK(elected.elected);
    CHECK(slave.masterClockIdentity != 0);
    CHECK(slave.grandmasterIdentity == slave.masterClockIdentity);

    for (uint16_t sequenceId = 1; sequenceId <= 8; ++sequenceId) {
        const auto message = sync(port, sequenceId);
        const SlaveVerdict verdict = feed(slave, message.data(), message.size(), 319);
        INFO("Sync ", sequenceId, " refused because: ", verdict.reason);
        CHECK(verdict.used);
        CHECK_FALSE(verdict.lockReset);
    }
    CHECK(slave.locked);
}

TEST_CASE("Our Sync is two-step, so the module waits for the Follow_Up") {
    const PortContext port = masterPort();
    const auto message = sync(port, 1);
    CHECK(syncIsTwoStep(message.data()));

    SlaveState slave;
    give(slave, announce(port, 1));
    const auto follow = followUp(port, 1, 1'700'000'000'000'000'000ull);
    const SlaveVerdict verdict = give(slave, follow);
    INFO("refused because: ", verdict.reason);
    CHECK(verdict.used);
}

TEST_CASE("An Announce on another domain is dropped, and no master is elected") {
    SlaveState slave;
    slave.configuredDomain = 0;

    const SlaveVerdict verdict = give(slave, announce(masterPort(109), 1));

    CHECK(verdict.used);          // read
    CHECK_FALSE(verdict.elected); // and put down
    CHECK(slave.masterClockIdentity == 0);
    CHECK(verdict.reason.find("domain") != std::string::npos);
}

TEST_CASE("A slave configured for the Dolby domain takes only that domain") {
    SlaveState slave;
    slave.configuredDomain = 109;

    CHECK_FALSE(give(slave, announce(masterPort(0), 1)).elected);
    CHECK(give(slave, announce(masterPort(109), 2)).elected);
}

TEST_CASE("A Sync from another clock is dropped") {
    const PortContext ours = masterPort();
    SlaveState slave;
    give(slave, announce(ours, 1));

    PortContext other = ours;
    other.clockIdentity.id = {0xB8, 0x27, 0xEB, 0xFF, 0xFE, 0x09, 0x09, 0x09};

    const SlaveVerdict verdict = give(slave, sync(other, 2), 319);
    CHECK_FALSE(verdict.used);
    CHECK(verdict.reason.find("not the elected master") != std::string::npos);
}

TEST_CASE("A gap wider than the hysteresis resets the lock, a narrower one does not") {
    const PortContext port = masterPort();
    SlaveState slave;
    give(slave, announce(port, 1));
    give(slave, sync(port, 1), 319);
    give(slave, sync(port, 2), 319);
    REQUIRE(slave.locked);

    SUBCASE("four apart, which is the module's own hysteresis, keeps it") {
        const SlaveVerdict verdict = give(slave, sync(port, 6), 319);
        CHECK(verdict.used);
        CHECK_FALSE(verdict.lockReset);
        CHECK(slave.locked);
    }

    SUBCASE("five apart drops it") {
        const SlaveVerdict verdict = give(slave, sync(port, 7), 319);
        CHECK(verdict.used);
        CHECK(verdict.lockReset);
        CHECK_FALSE(slave.locked);
    }
}

TEST_CASE("A truncated message is not read") {
    const PortContext port = masterPort();
    const auto message = announce(port, 1);

    SlaveState slave;
    const SlaveVerdict verdict = feed(slave, message.data(), kMinAnnounceBytes - 1, 320);
    CHECK_FALSE(verdict.used);
    CHECK(slave.masterClockIdentity == 0);
}

TEST_CASE("A packet off the PTP ports is not a PTP packet to the module") {
    const PortContext port = masterPort();
    const auto message = announce(port, 1);

    SlaveState slave;
    CHECK_FALSE(give(slave, message, 5004).used);
}

TEST_CASE("The profile's own rates clear the module's two timeouts") {
    // The module restarts its election after five seconds without an Announce
    // from the master (PTP.c:56) and expects a Sync at least every two
    // seconds (PTP.c:48). Both intervals are powers of two of a second.
    const auto interval = [](int8_t logInterval) {
        return logInterval >= 0 ? static_cast<double>(1u << logInterval)
                                : 1.0 / static_cast<double>(1u << -logInterval);
    };

    const auto& media = kPtpAes67MediaProfile.settings;
    CHECK(interval(media.logAnnounceInterval) * 1e9 < kAnnounceTimeout100ns * 100.0);
    CHECK(interval(media.logSyncInterval) * 1e9 < static_cast<double>(kSyncWatchdogNs));

    const auto& tight = kPtpAes67TightProfile.settings;
    CHECK(interval(tight.logAnnounceInterval) * 1e9 < kAnnounceTimeout100ns * 100.0);
    CHECK(interval(tight.logSyncInterval) * 1e9 < static_cast<double>(kSyncWatchdogNs));

    // The 1588 default profile is one Announce every two seconds and one Sync
    // a second: slower, and still inside both.
    const auto& standard = kPtpDefaultProfile.settings;
    CHECK(interval(standard.logAnnounceInterval) * 1e9 < kAnnounceTimeout100ns * 100.0);
    CHECK(interval(standard.logSyncInterval) * 1e9 < static_cast<double>(kSyncWatchdogNs));
}
