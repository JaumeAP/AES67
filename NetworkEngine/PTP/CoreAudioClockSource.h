//
// CoreAudioClockSource.h
// AES67 macOS Driver
// A PTPClockSource that genuinely disciplines to another local CoreAudio
// device's hardware clock — "run on this interface's sample clock" rather
// than this Mac's own free-running crystal.
//
// How it works: a word/sample clock has no wall-clock epoch to read, but
// the device's SAMPLE COUNTER advances at exactly the device's hardware
// rate. So this derives its nanoseconds from that counter —
//
//     ns = anchorNs + (deviceSampleTime - anchorSample) * 1e9 / nominalRate
//
// — which ticks at the device's real rate, not the Mac's. anchorNs ties it
// to a real time-of-day once, so PTP origin timestamps stay sensible while
// their RATE follows the device. If the device runs slightly fast, this
// clock runs slightly fast with it. That is true frequency syntonization,
// which earlier revisions of this class did not do (they returned
// CLOCK_REALTIME and only changed the advertised quality bits).
//
// To read the counter the device has to be running, so this installs its
// own no-op IOProc and starts the device — a deliberate side effect on the
// chosen reference device, documented rather than hidden.
//
// Honest boundary, unchanged: none of this is verified against real
// reference hardware here — the same "written, not hardware-verified"
// caveat the whole PTP path carries, which is why the driver ships with
// PTP off by default. If the device can't be read (absent, asleep, refuses
// to run), the source falls back to the Mac clock AND drops its clockClass
// to 248, so it never announces a lock it doesn't have.
//
#pragma once

#include "PTPClockSource.h"

#include <CoreAudio/CoreAudio.h>

#include <atomic>
#include <mutex>

namespace AES67 {

class CoreAudioClockSource : public PTPClockSource {
public:
    explicit CoreAudioClockSource(AudioDeviceID deviceID, std::string deviceName);
    ~CoreAudioClockSource() override;

    CoreAudioClockSource(const CoreAudioClockSource&) = delete;
    CoreAudioClockSource& operator=(const CoreAudioClockSource&) = delete;

    uint64_t currentTimeNs() const override;

    uint8_t clockClass() const override {
        // §7.6.2.4 Table 5: 13 = locked to an accurate external source.
        // Only claimed while we're actually deriving time from the device's
        // counter (locked_), never merely because the device exists.
        return locked_.load(std::memory_order_relaxed) ? 13 : 248;
    }

    PTPClockAccuracy clockAccuracy() const override {
        return locked_.load(std::memory_order_relaxed) ? PTPClockAccuracy::Within1Microsecond
                                                        : PTPClockAccuracy::Unknown;
    }

    std::string name() const override { return deviceName_ + " (local reference)"; }

    AudioDeviceID deviceID() const { return deviceID_; }

private:
    /// Installs a no-op IOProc and starts the device so its clock runs and
    /// AudioDeviceGetCurrentTime works. Idempotent.
    void ensureDeviceRunning() const;

    /// The device's nominal sample rate, or 0 on failure.
    double readNominalSampleRate() const;

    /// Keeps the device running; does nothing with the audio.
    static OSStatus nullIOProc(AudioObjectID, const AudioTimeStamp*, const AudioBufferList*,
                               const AudioTimeStamp*, AudioBufferList*, const AudioTimeStamp*, void*);

    AudioDeviceID deviceID_;
    std::string deviceName_;

    // All mutable: currentTimeNs() is const but disciplines lazily.
    mutable std::mutex mutex_;
    mutable AudioDeviceIOProcID ioProcID_{nullptr};
    mutable bool startedDevice_{false};
    mutable bool haveAnchor_{false};
    mutable double anchorSample_{0.0};
    mutable uint64_t anchorNs_{0};
    mutable double nominalRate_{0.0};
    // Last value currentTimeNs() returned. The clamp point that keeps the
    // emitted timeline monotonic across dropouts and re-anchors — see the
    // `emit` lambda in currentTimeNs().
    mutable uint64_t lastEmittedNs_{0};
    mutable std::atomic<bool> locked_{false};
};

} // namespace AES67
