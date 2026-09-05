//
// AudioClockDeviceList.h
// AES67 macOS Driver
// Enumerates local CoreAudio devices that expose their own hardware clock
// domain — candidates to lock a CoreAudioClockSource to. Backs the "select a
// clock source" list: internal, or one of these.
//
#pragma once

#include <CoreAudio/CoreAudio.h>

#include <string>
#include <vector>

namespace AES67 {

struct AudioClockDeviceInfo {
    AudioDeviceID deviceID;
    std::string uid;
    std::string name;
};

/// Lists CoreAudio devices with a nonzero clock domain (their own
/// independent hardware clock — the CoreAudio convention for "this isn't a
/// software-only device riding on someone else's timing"), excluding
/// `excludeDeviceID` (pass this driver's own AES67 device here: it has no
/// hardware clock of its own to offer as a reference, and offering it back
/// to itself would be circular).
std::vector<AudioClockDeviceInfo> listClockCapableAudioDevices(
    AudioDeviceID excludeDeviceID = kAudioObjectUnknown);

/// Resolves a persisted device UID (PTPMasterSettings::lockToDeviceUID) back
/// to a live AudioDeviceID — UIDs are stable across reboots/replugs,
/// AudioDeviceIDs aren't, so that's what gets saved to disk; this is the
/// other half, done once at driver startup. Returns kAudioObjectUnknown if
/// the UID doesn't currently resolve to any connected device (unplugged,
/// or the setting predates whatever's attached now).
AudioDeviceID resolveAudioDeviceUID(const std::string& uid);

} // namespace AES67
