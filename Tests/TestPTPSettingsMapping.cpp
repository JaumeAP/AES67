//
// TestPTPSettingsMapping.cpp
// AES67 macOS Driver
//
// What the installation configured has to arrive at the engines that put
// it on the wire. The dataset is settable so that a driver can slot into
// an existing clock hierarchy; a setting that is stored, shown in the app
// and then dropped on the way to PTPSlave or PTPMaster is worse than not
// having it, because it looks like it took.
//
// applyPTPSettings is a free function precisely so this can be checked
// without opening a socket or waiting for a lock.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/PTP/PTPDInterface.h"

using namespace AES67;

TEST_CASE("The Configured Dataset Reaches Both Engines") {
    PTPMasterSettings settings;
    settings.priority1 = 100;
    settings.priority2 = 90;
    settings.syncIntervalMs = 250;
    settings.announceIntervalMs = 2000;
    settings.delayReqIntervalMs = 125;
    settings.delayMechanism = "p2p";
    settings.dscp = 46;

    PTPSlaveConfig slave;
    PTPMasterConfig master;
    applyPTPSettings(settings, slave, master);

    CHECK(slave.delayReqIntervalMs == 125);
    CHECK(slave.delayMechanism == DelayMechanism::PeerToPeer);
    CHECK(slave.dscp == 46);

    CHECK(master.priority1 == 100);
    CHECK(master.priority2 == 90);
    CHECK(master.syncIntervalMs == 250);
    CHECK(master.announceIntervalMs == 2000);
    CHECK(master.dscp == 46);
}

TEST_CASE("The Defaults Leave Everything Where The Code Had It") {
    const PTPMasterSettings defaults;

    PTPSlaveConfig slave;
    PTPMasterConfig master;
    const PTPSlaveConfig slaveBefore;
    const PTPMasterConfig masterBefore;
    applyPTPSettings(defaults, slave, master);

    // A file from an older build carries none of these fields, so the
    // struct hands over its defaults — and those are the values the code
    // had compiled in. Nothing moves.
    CHECK(slave.delayReqIntervalMs == slaveBefore.delayReqIntervalMs);
    CHECK(slave.delayMechanism == slaveBefore.delayMechanism);
    CHECK(slave.dscp == slaveBefore.dscp);
    CHECK(master.priority1 == masterBefore.priority1);
    CHECK(master.priority2 == masterBefore.priority2);
    CHECK(master.syncIntervalMs == masterBefore.syncIntervalMs);
    CHECK(master.announceIntervalMs == masterBefore.announceIntervalMs);
    CHECK(master.dscp == masterBefore.dscp);
}

TEST_CASE("Anything That Is Not p2p Is End To End") {
    PTPSlaveConfig slave;
    PTPMasterConfig master;

    PTPMasterSettings settings;
    settings.delayMechanism = "e2e";
    applyPTPSettings(settings, slave, master);
    CHECK(slave.delayMechanism == DelayMechanism::EndToEnd);

    // A hand-edited file can hold anything; AES67 is end to end, so that
    // is where an unreadable value lands.
    settings.delayMechanism = "peer-to-peer-ish";
    applyPTPSettings(settings, slave, master);
    CHECK(slave.delayMechanism == DelayMechanism::EndToEnd);
}
