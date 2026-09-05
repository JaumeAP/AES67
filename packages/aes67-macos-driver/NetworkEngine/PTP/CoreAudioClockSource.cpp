//
// CoreAudioClockSource.cpp
// AES67 macOS Driver
// See CoreAudioClockSource.h for the design and the honest caveats.
//
#include "CoreAudioClockSource.h"
#include <cmath>

#include "Driver/DebugLog.h"

namespace AES67 {

CoreAudioClockSource::CoreAudioClockSource(AudioDeviceID deviceID, std::string deviceName)
    : deviceID_(deviceID), deviceName_(std::move(deviceName)) {}

CoreAudioClockSource::~CoreAudioClockSource() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ioProcID_) {
        if (startedDevice_) {
            AudioDeviceStop(deviceID_, ioProcID_);
        }
        AudioDeviceDestroyIOProcID(deviceID_, ioProcID_);
        ioProcID_ = nullptr;
    }
}

OSStatus CoreAudioClockSource::nullIOProc(AudioObjectID, const AudioTimeStamp*,
                                          const AudioBufferList*, const AudioTimeStamp*,
                                          AudioBufferList*, const AudioTimeStamp*, void*) {
    // Do nothing. The device only needs to be *running* for its clock to
    // advance and AudioDeviceGetCurrentTime to work; we consume no audio.
    return noErr;
}

double CoreAudioClockSource::readNominalSampleRate() const {
    Float64 rate = 0.0;
    UInt32 size = sizeof(rate);
    AudioObjectPropertyAddress addr{
        kAudioDevicePropertyNominalSampleRate,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    if (AudioObjectGetPropertyData(deviceID_, &addr, 0, nullptr, &size, &rate) != noErr) {
        return 0.0;
    }
    return static_cast<double>(rate);
}

void CoreAudioClockSource::ensureDeviceRunning() const {
    // Caller holds mutex_.
    if (ioProcID_) return; // already set up

    OSStatus st = AudioDeviceCreateIOProcID(deviceID_, &CoreAudioClockSource::nullIOProc,
                                            const_cast<CoreAudioClockSource*>(this), &ioProcID_);
    if (st != noErr || !ioProcID_) {
        ioProcID_ = nullptr;
        AES67_LOGF("CoreAudioClockSource: could not create IOProc for '%s' (err %d) — "
                   "falling back to the Mac clock", deviceName_.c_str(), static_cast<int>(st));
        return;
    }

    st = AudioDeviceStart(deviceID_, ioProcID_);
    if (st != noErr) {
        AudioDeviceDestroyIOProcID(deviceID_, ioProcID_);
        ioProcID_ = nullptr;
        AES67_LOGF("CoreAudioClockSource: could not start '%s' (err %d) — "
                   "falling back to the Mac clock", deviceName_.c_str(), static_cast<int>(st));
        return;
    }
    startedDevice_ = true;
    AES67_LOGF("CoreAudioClockSource: disciplining to '%s'", deviceName_.c_str());
}

uint64_t CoreAudioClockSource::currentTimeNs() const {
    std::lock_guard<std::mutex> lock(mutex_);

    ensureDeviceRunning();

    // Never hand back a time earlier than one we've already emitted: a PTP
    // master whose Sync origin timestamp goes backwards is a real protocol
    // defect, and several paths below (fallback to the Mac clock, and every
    // re-anchor) would otherwise do exactly that — the device-derived
    // timeline runs ahead of (or behind) wall-clock by the accumulated
    // frequency error, so switching between the two mid-stream jumps. This
    // clamps every return through one point.
    auto emit = [this](uint64_t ns) -> uint64_t {
        if (ns < lastEmittedNs_) ns = lastEmittedNs_;
        lastEmittedNs_ = ns;
        return ns;
    };

    // If the device won't run, there's nothing to lock to — be the Mac
    // clock and say so (locked_ false => clockClass 248). Clamped, so a
    // device that was running ahead doesn't drag the timeline backwards
    // when it drops out; it freezes until the Mac clock catches up or the
    // device returns.
    if (!ioProcID_ || !startedDevice_) {
        locked_.store(false, std::memory_order_relaxed);
        haveAnchor_ = false;
        return emit(ptpSystemTimeNs());
    }

    AudioTimeStamp ts{};
    OSStatus st = AudioDeviceGetCurrentTime(deviceID_, &ts);
    const bool sampleValid = (ts.mFlags & kAudioTimeStampSampleTimeValid) != 0;
    if (st != noErr || !sampleValid) {
        // Device stopped, unplugged, or slept between calls. Drop the lock,
        // and force a fresh anchor if it comes back.
        locked_.store(false, std::memory_order_relaxed);
        haveAnchor_ = false;
        return emit(ptpSystemTimeNs());
    }

    const double sampleTime = ts.mSampleTime;

    const double rate = readNominalSampleRate();
    if (rate <= 0.0) {
        locked_.store(false, std::memory_order_relaxed);
        haveAnchor_ = false;
        return emit(ptpSystemTimeNs());
    }

    // (Re)anchor on: first read, a nominal-rate change (the scale factor
    // depends on it), or the device's sample counter going backwards
    // (engine restart/reconfig that didn't surface an error). That last
    // guard also keeps elapsedSamples below non-negative, so the cast to
    // uint64_t can never see a negative double (UB).
    if (!haveAnchor_ || rate != nominalRate_ || sampleTime < anchorSample_) {
        anchorSample_ = sampleTime;
        // Continue the timeline from where it left off rather than snapping
        // to wall-clock — carrying lastEmittedNs_ forward preserves both
        // monotonicity and the accumulated device offset across the
        // re-anchor. Only the very first anchor (nothing emitted yet) ties
        // to real time-of-day.
        anchorNs_ = (lastEmittedNs_ != 0) ? lastEmittedNs_ : ptpSystemTimeNs();
        nominalRate_ = rate;
        haveAnchor_ = true;
        locked_.store(true, std::memory_order_relaxed);
        return emit(anchorNs_);
    }

    // Nanoseconds derived from the DEVICE's sample counter: this is the
    // syntonization. (sampleTime - anchorSample) is device samples elapsed
    // (>= 0, guaranteed by the re-anchor guard above); divided by the
    // nominal rate and scaled to ns, it advances at the device's real
    // hardware rate, drifting from anchorNs_ by exactly the device-vs-Mac
    // frequency error — which is the whole point.
    const double elapsedSamples = sampleTime - anchorSample_;
    const double elapsedNs = (elapsedSamples / nominalRate_) * 1.0e9;
    locked_.store(true, std::memory_order_relaxed);
    // lround rather than a cast of (x + 0.5): elapsedNs is derived from a
    // sample count over a rate, so it is non-negative in practice, but the cast
    // rounds the wrong way for negatives and nothing here enforces that.
    return emit(anchorNs_ + static_cast<uint64_t>(std::llround(elapsedNs)));
}

} // namespace AES67
