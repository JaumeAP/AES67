//
// TestAudioClockDevices.cpp
// AES67 macOS Driver
//
// The list of local devices that could be a PTP reference, and the clock
// source that locks to one. Both were at 0% of 191 lines: they are compiled
// into five suites and exercised by none.
//
// What is checked here is everything that does not touch a device's audio.
// CoreAudioClockSource, when it is pointed at a real device, installs a no-op
// IOProc and starts it -- a deliberate side effect on somebody's audio
// hardware, documented in its own header -- so the cases below use
// kAudioObjectUnknown, which is the fallback path and the one that runs on
// every machine with nothing suitable attached.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/PTP/AudioClockDeviceList.h"
#include "NetworkEngine/PTP/CoreAudioClockSource.h"

#include <string>
#include <vector>

using namespace AES67;

TEST_CASE("Every device offered as a reference can be identified again") {
    // The list is what ManagerApp shows; the UID is what gets written to
    // disk, because an AudioDeviceID is not stable across a reboot or a
    // replug and a UID is. A device offered with a UID that does not resolve
    // is one the user can pick and the driver cannot find afterwards.
    const std::vector<AudioClockDeviceInfo> devices = listClockCapableAudioDevices();

    for (const auto& device : devices) {
        INFO("device: " << device.name << " (" << device.uid << ")");
        CHECK(device.deviceID != kAudioObjectUnknown);
        CHECK_FALSE(device.uid.empty());
        CHECK_FALSE(device.name.empty());
        CHECK(resolveAudioDeviceUID(device.uid) == device.deviceID);
    }
}

TEST_CASE("The excluded device is the only one missing") {
    const std::vector<AudioClockDeviceInfo> all = listClockCapableAudioDevices();
    if (all.empty()) {
        // Nothing with a clock domain of its own is attached. That is a
        // legitimate machine, and the exclusion has nothing to do.
        return;
    }

    // The driver passes its own device here: it has no hardware clock to
    // offer, and offering it back to itself would be circular.
    const AudioDeviceID excluded = all.front().deviceID;
    const std::vector<AudioClockDeviceInfo> rest = listClockCapableAudioDevices(excluded);

    CHECK(rest.size() == all.size() - 1);
    for (const auto& device : rest) {
        CHECK(device.deviceID != excluded);
    }
}

TEST_CASE("Asking twice answers the same") {
    // It is called from a settings screen that reopens, and from the driver
    // at start. Two calls with nothing plugged or unplugged between them have
    // to agree, or the list reorders under the selection.
    const auto first = listClockCapableAudioDevices();
    const auto second = listClockCapableAudioDevices();

    REQUIRE(first.size() == second.size());
    for (size_t i = 0; i < first.size(); ++i) {
        CHECK(first[i].deviceID == second[i].deviceID);
        CHECK(first[i].uid == second[i].uid);
    }
}

TEST_CASE("A UID that names nothing resolves to nothing") {
    // The setting outlives the device: unplug the interface it named, or
    // restore a settings file from another machine, and this is the answer.
    // Anything else would have the driver lock to whatever device happened to
    // answer.
    CHECK(resolveAudioDeviceUID("") == kAudioObjectUnknown);
    CHECK(resolveAudioDeviceUID("no-such-device-uid") == kAudioObjectUnknown);
    CHECK(resolveAudioDeviceUID("AppleHDAEngineOutput:1B,0,1,1:0:not-real")
          == kAudioObjectUnknown);
}

TEST_CASE("A clock source with no device announces a free-running clock") {
    // The fallback, which is what runs when the chosen device is absent,
    // asleep or refuses to start. 248 is IEEE 1588 SS 7.6.2.4's "default, not
    // synchronized to a primary reference" -- the same thing InternalClockSource
    // announces, and the point is that this one never claims 13 merely
    // because a device was named.
    CoreAudioClockSource source(kAudioObjectUnknown, "Nothing attached");

    CHECK(source.deviceID() == kAudioObjectUnknown);
    CHECK(source.clockClass() == 248);
    CHECK(source.clockAccuracy() == PTPClockAccuracy::Unknown);
}

TEST_CASE("It says which device it is, in a name a person reads") {
    // ManagerApp shows this under "currently locked to".
    CoreAudioClockSource source(kAudioObjectUnknown, "Scarlett 18i20");

    const std::string name = source.name();
    CHECK(name.find("Scarlett 18i20") != std::string::npos);
    CHECK_FALSE(name.empty());
}

TEST_CASE("Its time moves forward and never back") {
    // PTPMaster stamps this into every Sync and Follow_Up. A timeline that
    // went backwards -- across a re-anchor, or a dropout on the reference --
    // is one a slave reads as an enormous negative offset.
    CoreAudioClockSource source(kAudioObjectUnknown, "Nothing attached");

    uint64_t previous = source.currentTimeNs();
    CHECK(previous > 0);

    for (int i = 0; i < 100; ++i) {
        const uint64_t now = source.currentTimeNs();
        CHECK(now >= previous);
        previous = now;
    }
}
