//
// TestPTPArbitrator.cpp
// AES67 macOS Driver
//
// The piece that decides which of PTPMaster and PTPSlave is doing something,
// and what the UI is told about it. 88 lines that nothing reached: there was
// no suite for this file at all.
//
// Offline, like TestPTPMaster: mostly constructed and asked rather than run.
// The one case that calls start() puts it on loopback and on the high ports
// AES67PTPStressRun uses, so nothing it sends leaves this machine and nothing
// it binds is wanted by another suite. What a PTP exchange then does belongs
// with the loopback tier.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/PTP/PTPArbitrator.h"
#include "NetworkEngine/PTP/PTPClockSource.h"
#include "NetworkEngine/PTP/PTPDiagnostics.h"

using namespace AES67;

namespace {

/// The configuration the driver builds when nothing has been chosen: this
/// Mac's own clock, domain 0, and whatever interface it detected.
PTPArbitratorConfig defaultConfig() {
    PTPArbitratorConfig config;
    config.domain = 0;
    config.interfaceName = "lo0";
    config.clockSourceKind = PTPClockSourceKind::Internal;
    return config;
}

} // namespace

TEST_CASE("An Arbitrator Starts As Slave And Runs Nothing") {
    PTPArbitrator arbitrator(defaultConfig());

    // PTPMaster begins Listening, not Master, so the arbitrator's answer is
    // Slave: 1588 has a port listen before it announces, and the role this
    // reports is the master's verdict rather than a wish.
    CHECK(arbitrator.role() == PTPRole::Slave);
}

TEST_CASE("Diagnostics Before Anything Has Happened Say So") {
    PTPArbitrator arbitrator(defaultConfig());

    PTPDiagnostics diag;
    arbitrator.updateDiagnostics(diag);

    // Not master, nothing heard: the slave fills these in, and what it has
    // to say before a single Announce has arrived is that it is not locked
    // to anybody.
    CHECK(diag.role == PTPDiagnostics::Role::Slave);
    CHECK_FALSE(diag.everWasMaster);
    CHECK_FALSE(diag.hasCompetitor);
    CHECK(diag.competitorPriority1 == 0);
    CHECK(diag.competitorPriority2 == 0);
}

TEST_CASE("Diagnostics Are Idempotent") {
    PTPArbitrator arbitrator(defaultConfig());

    // The UI calls this on a timer. Asking twice has to answer twice the
    // same, and must not accumulate anything -- everWasMaster is the one
    // field that latches, and it latches on being master, not on being asked.
    PTPDiagnostics first;
    PTPDiagnostics second;
    arbitrator.updateDiagnostics(first);
    arbitrator.updateDiagnostics(second);

    CHECK(first.role == second.role);
    CHECK(first.everWasMaster == second.everWasMaster);
    CHECK(first.hasCompetitor == second.hasCompetitor);
}

TEST_CASE("A Measurement Callback Set Before Start Is Kept") {
    PTPArbitrator arbitrator(defaultConfig());

    // The driver sets this once, at construction, and the slave that will
    // deliver the measurements may not be running yet. Setting it must not
    // require a running slave, and must not throw when there is none.
    int calls = 0;
    arbitrator.setMeasurementCallback([&calls](const PTPMeasurement&) { ++calls; });

    // Nothing has measured anything: the callback exists and has not fired.
    CHECK(calls == 0);

    // And replacing it is allowed, which is what a profile change does.
    arbitrator.setMeasurementCallback(nullptr);
    CHECK(calls == 0);
}

TEST_CASE("Stopping One That Never Started Is Not An Error") {
    PTPArbitrator arbitrator(defaultConfig());

    // ~PTPArbitrator calls stop(), and AES67Device's teardown calls it too:
    // the second of those can run without the first ever having started.
    arbitrator.stop();
    CHECK(arbitrator.role() == PTPRole::Slave);
}

TEST_CASE("A Locked Device Kind With No Device Falls Back To The Internal Clock") {
    PTPArbitratorConfig config = defaultConfig();
    config.clockSourceKind = PTPClockSourceKind::LocalAudioDevice;
    config.lockToDeviceID = kAudioObjectUnknown;  // nothing selected

    // The constructor's own branch: asked to lock to a device and given none,
    // it builds an InternalClockSource rather than a CoreAudioClockSource
    // pointed at nothing. Reaching it needs no audio hardware, which is the
    // point of testing it here.
    PTPArbitrator arbitrator(config);
    CHECK(arbitrator.role() == PTPRole::Slave);
}

TEST_CASE("The Chosen Clock Source Is The One Announced") {
    PTPArbitrator arbitrator(defaultConfig());

    // What ManagerApp's "currently locked to" reads, and what PTPMaster
    // stamps into Announce: an unconfigured driver serves this Mac's own
    // free-running clock, which IEEE 1588 §7.6.2.4 numbers 248 and whose
    // accuracy is honestly Unknown rather than a figure nobody measured.
    const PTPClockSource& source = arbitrator.clockSource();
    CHECK(source.clockClass() == 248);
    CHECK(source.clockAccuracy() == PTPClockAccuracy::Unknown);
    CHECK_FALSE(source.name().empty());
    CHECK(source.currentTimeNs() > 0);
}

TEST_CASE("A Locked Device Kind With No Device Still Announces The Internal Clock") {
    PTPArbitratorConfig config = defaultConfig();
    config.clockSourceKind = PTPClockSourceKind::LocalAudioDevice;
    config.lockToDeviceID = kAudioObjectUnknown;

    // The fallback the constructor takes is not just "some source": it has
    // to be the internal one, because announcing a locked device's accuracy
    // while locked to nothing is the failure that matters here.
    PTPArbitrator arbitrator(config);
    CHECK(arbitrator.clockSource().clockClass() == 248);
}

TEST_CASE("One That Never Started Is Not Running And Is Not Locked") {
    PTPArbitrator arbitrator(defaultConfig());

    // Both are read by the diagnostics UI on a timer, before anything has
    // been started and after everything has been stopped.
    CHECK_FALSE(arbitrator.isRunning());
    CHECK_FALSE(arbitrator.isSlaveLocked());
}

TEST_CASE("Starting And Stopping Is Symmetric, On Ports Nothing Else Uses") {
    // This case used to be written as "starting without the privilege to bind
    // 319 fails cleanly", on the assumption that an unprivileged process
    // cannot have those ports. On macOS it can -- README.md:142 measured it --
    // so the refusal branch was never taken and what actually happened was a
    // real PTP master transmitting Announce and Sync, on the two ports
    // TestPTPMaster, PTPLoopback and PTPService also want.
    //
    // Loopback and the high ports AES67PTPStressRun uses, so the exchange
    // stays on this machine and contends with nothing. What is checked is the
    // invariant either outcome has to satisfy: isRunning() agrees with what
    // start() returned, and stop() is symmetric and repeatable.
    PTPArbitratorConfig config = defaultConfig();
    config.masterConfig.eventPort = 20319;
    config.masterConfig.generalPort = 20320;
    config.slaveConfig.eventPort = 20319;
    config.slaveConfig.generalPort = 20320;

    PTPArbitrator arbitrator(config);

    const bool started = arbitrator.start();
    CHECK(arbitrator.isRunning() == started);

    if (started) {
        // A second start is refused while the first is up.
        CHECK_FALSE(arbitrator.start());
    } else {
        // Nothing was left half-built: no thread to join, no socket open.
        CHECK(arbitrator.role() == PTPRole::Slave);
        CHECK_FALSE(arbitrator.isSlaveLocked());
    }

    arbitrator.stop();
    CHECK_FALSE(arbitrator.isRunning());
    arbitrator.stop();
    CHECK_FALSE(arbitrator.isRunning());
}
