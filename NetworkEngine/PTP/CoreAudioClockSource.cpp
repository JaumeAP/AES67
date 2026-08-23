//
// CoreAudioClockSource.cpp
// AES67 macOS Driver
// See CoreAudioClockSource.h for the design and the honest caveats.
//
#include "CoreAudioClockSource.h"

#include "../../Driver/DebugLog.h"

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

    // If the device won't run, there's nothing to lock to — be the Mac
    // clock and say so (locked_ false => clockClass 248).
    if (!ioProcID_ || !startedDevice_) {
        locked_.store(false, std::memory_order_relaxed);
        haveAnchor_ = false;
        return ptpSystemTimeNs();
    }

    AudioTimeStamp ts{};
    OSStatus st = AudioDeviceGetCurrentTime(deviceID_, &ts);
    const bool sampleValid = (ts.mFlags & kAudioTimeStampSampleTimeValid) != 0;
    if (st != noErr || !sampleValid) {
        // Device stopped, unplugged, or slept between calls. Drop the lock,
        // and force a fresh anchor if it comes back.
        locked_.store(false, std::memory_order_relaxed);
        haveAnchor_ = false;
        return ptpSystemTimeNs();
    }

    const double sampleTime = ts.mSampleTime;

    // (Re)anchor on first read, after a dropout, or if the device's nominal
    // rate changed under us — the scale factor below depends on it.
    const double rate = readNominalSampleRate();
    if (rate <= 0.0) {
        locked_.store(false, std::memory_order_relaxed);
        haveAnchor_ = false;
        return ptpSystemTimeNs();
    }

    if (!haveAnchor_ || rate != nominalRate_) {
        anchorSample_ = sampleTime;
        anchorNs_ = ptpSystemTimeNs(); // tie to real time-of-day once
        nominalRate_ = rate;
        haveAnchor_ = true;
        locked_.store(true, std::memory_order_relaxed);
        return anchorNs_;
    }

    // Nanoseconds derived from the DEVICE's sample counter: this is the
    // syntonization. (sampleTime - anchorSample) is device samples elapsed;
    // divided by the nominal rate and scaled to ns, it advances at the
    // device's real hardware rate, drifting from anchorNs_ by exactly the
    // device-vs-Mac frequency error — which is the whole point.
    const double elapsedSamples = sampleTime - anchorSample_;
    const double elapsedNs = (elapsedSamples / nominalRate_) * 1.0e9;
    locked_.store(true, std::memory_order_relaxed);
    return anchorNs_ + static_cast<uint64_t>(elapsedNs + 0.5);
}

} // namespace AES67
