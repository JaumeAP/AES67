//
// CoreAudioClockSource.h
// AES67 macOS Driver
// A PTPClockSource backed by another local CoreAudio device — "lock to this
// audio interface's clock" rather than this Mac's own free-running crystal.
//
// Honest about what this does and doesn't do: word-clock/sample-clock
// devices give a stable SAMPLE RATE reference, not an absolute time-of-day —
// there's no wall-clock epoch to read off a word clock. So currentTimeNs()
// still returns this Mac's own CLOCK_REALTIME (there's nothing else to
// return); what locking to the device buys is a *quality claim*, not a
// different time value: while the device is confirmed present and exposing
// its own clock domain, this source announces a tighter clockAccuracy than
// InternalClockSource. If the device disappears (unplugged, sleep), the
// claim automatically drops back to the internal-clock default — it never
// asserts a quality it can't currently verify.
//
// What this does NOT do (documented, not hidden): actual frequency
// syntonization — steering the Mac's clock rate to track the device's
// sample clock via a PLL. That's real additional work, and unverifiable
// here without the reference hardware attached. See
// aes67_driver_macos_platform_audit.md in new_renderer/docs for the same
// kind of "written, not verified against real hardware" caveat already
// carried by PTPSlave's own network sync path.
//
#pragma once

#include "PTPClockSource.h"

#include <CoreAudio/CoreAudio.h>

namespace AES67 {

class CoreAudioClockSource : public PTPClockSource {
public:
    explicit CoreAudioClockSource(AudioDeviceID deviceID, std::string deviceName)
        : deviceID_(deviceID), deviceName_(std::move(deviceName)) {}

    uint64_t currentTimeNs() const override { return ptpSystemTimeNs(); }

    uint8_t clockClass() const override {
        // §7.6.2.4 Table 5: 13 = "Application Specific" time source, locked
        // to an accurate external source. Only while we can currently
        // confirm the device is alive and has its own clock domain — see
        // isDeviceLocked() below.
        return isDeviceLocked() ? 13 : 248;
    }

    PTPClockAccuracy clockAccuracy() const override {
        return isDeviceLocked() ? PTPClockAccuracy::Within1Microsecond
                                 : PTPClockAccuracy::Unknown;
    }

    std::string name() const override { return deviceName_ + " (local reference)"; }

    AudioDeviceID deviceID() const { return deviceID_; }

private:
    /// True while the device is present, running, and reports a nonzero
    /// clock domain (kAudioDevicePropertyClockDomain — zero means "no
    /// independent hardware clock", per CoreAudio's own convention for
    /// software-only devices; this driver's own AES67 device is one such
    /// case, which is exactly why it's excluded from the candidate list —
    /// see AudioClockDeviceList.h).
    bool isDeviceLocked() const {
        UInt32 isAlive = 0;
        UInt32 size = sizeof(isAlive);
        AudioObjectPropertyAddress aliveAddr{
            kAudioDevicePropertyDeviceIsAlive,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        OSStatus status = AudioObjectGetPropertyData(deviceID_, &aliveAddr, 0, nullptr,
                                                       &size, &isAlive);
        if (status != noErr || isAlive == 0) return false;

        UInt32 clockDomain = 0;
        size = sizeof(clockDomain);
        AudioObjectPropertyAddress domainAddr{
            kAudioDevicePropertyClockDomain,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        status = AudioObjectGetPropertyData(deviceID_, &domainAddr, 0, nullptr,
                                             &size, &clockDomain);
        return status == noErr && clockDomain != 0;
    }

    AudioDeviceID deviceID_;
    std::string deviceName_;
};

} // namespace AES67
