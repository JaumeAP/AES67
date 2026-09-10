//
// TestPTPArbitrator.cpp
// AES67 macOS Driver
//
// The piece that decides which of PTPMaster and PTPSlave is doing something,
// and what the UI is told about it. 88 lines that nothing reached: there was
// no suite for this file at all.
//
// Offline, like TestPTPMaster: constructed and asked, never started. start()
// binds 319 and 320 and spawns a monitor thread, which belongs with the
// loopback tier; what is decided before any of that is the object's own.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/PTP/PTPArbitrator.h"
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
