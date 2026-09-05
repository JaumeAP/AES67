//
// HALValidate.cpp
// Core Audio HAL conformance check for the AES67 AudioServerPlugIn.
//
// Exercises the driver through Apple's own HAL client API — the same
// AudioObjectGetPropertyData / AudioDeviceStart path HALLab and coreaudiod
// use — rather than through the driver's internal classes. Nothing here
// links against the driver: it talks to whatever coreaudiod loaded, so it is
// a check of the installed plugin, not of this source tree.
//
// Sections: property contract, stream formats, sample-rate negotiation,
// buffer-size negotiation, and a live IOProc run. Exit status is non-zero if
// any check failed.
//
// Build: part of the Tools target (macOS only).
// Usage: ./Tools/HALValidate [--uid <device-uid>] [--name <substring>]
//                            [--list] [--seconds <n>] [--skip-io]
//                            [--skip-rates] [--verbose]
//

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

// Defined in HALValidateAudioAccess.mm: 1 authorized, 2 denied, 0 undecided.
extern "C" int HALValidateMicrophoneAuthorization(void);

namespace {

constexpr const char* kDefaultDeviceUID = "com.aes67.driver.device";

// ---------------------------------------------------------------------------
// Result collection
// ---------------------------------------------------------------------------

struct Check {
    std::string name;
    bool passed = false;
    bool skipped = false;
    std::string detail;
};

std::vector<Check> g_checks;
bool g_verbose = false;

void Record(const std::string& name, bool passed, const std::string& detail) {
    g_checks.push_back({name, passed, false, detail});
    printf("  [%s] %-38s %s\n", passed ? "PASS" : "FAIL", name.c_str(),
           detail.c_str());
}

void Skip(const std::string& name, const std::string& detail) {
    g_checks.push_back({name, false, true, detail});
    printf("  [SKIP] %-38s %s\n", name.c_str(), detail.c_str());
}

std::string OSStatusString(OSStatus status) {
    char code[5] = {0};
    UInt32 be = CFSwapInt32HostToBig(static_cast<UInt32>(status));
    memcpy(code, &be, 4);
    bool printable = true;
    for (int i = 0; i < 4; ++i) {
        if (code[i] < 32 || code[i] > 126) printable = false;
    }
    char buffer[64];
    if (printable) {
        snprintf(buffer, sizeof(buffer), "OSStatus %d ('%s')",
                 static_cast<int>(status), code);
    } else {
        snprintf(buffer, sizeof(buffer), "OSStatus %d", static_cast<int>(status));
    }
    return buffer;
}

// ---------------------------------------------------------------------------
// Property helpers
// ---------------------------------------------------------------------------

AudioObjectPropertyAddress Address(AudioObjectPropertySelector selector,
                                   AudioObjectPropertyScope scope
                                       = kAudioObjectPropertyScopeGlobal) {
    return {selector, scope, kAudioObjectPropertyElementMain};
}

OSStatus PropertySize(AudioObjectID object,
                      const AudioObjectPropertyAddress& address,
                      UInt32& size) {
    return AudioObjectGetPropertyDataSize(object, &address, 0, nullptr, &size);
}

template <typename T>
OSStatus GetScalar(AudioObjectID object,
                   const AudioObjectPropertyAddress& address, T& out) {
    UInt32 size = sizeof(T);
    return AudioObjectGetPropertyData(object, &address, 0, nullptr, &size, &out);
}

OSStatus GetString(AudioObjectID object,
                   const AudioObjectPropertyAddress& address,
                   std::string& out) {
    CFStringRef value = nullptr;
    UInt32 size = sizeof(value);
    OSStatus status =
        AudioObjectGetPropertyData(object, &address, 0, nullptr, &size, &value);
    if (status != noErr || value == nullptr) return status;
    char buffer[512] = {0};
    CFStringGetCString(value, buffer, sizeof(buffer), kCFStringEncodingUTF8);
    CFRelease(value);
    out = buffer;
    return noErr;
}

template <typename T>
OSStatus GetArray(AudioObjectID object,
                  const AudioObjectPropertyAddress& address,
                  std::vector<T>& out) {
    UInt32 size = 0;
    OSStatus status = PropertySize(object, address, size);
    if (status != noErr) return status;
    out.resize(size / sizeof(T));
    if (out.empty()) return noErr;
    return AudioObjectGetPropertyData(object, &address, 0, nullptr, &size,
                                      out.data());
}

// ---------------------------------------------------------------------------
// Device discovery
// ---------------------------------------------------------------------------

std::vector<AudioObjectID> AllDevices() {
    std::vector<AudioObjectID> devices;
    GetArray(kAudioObjectSystemObject, Address(kAudioHardwarePropertyDevices),
             devices);
    return devices;
}

std::string DeviceName(AudioObjectID device) {
    std::string name;
    GetString(device, Address(kAudioObjectPropertyName), name);
    return name;
}

std::string DeviceUID(AudioObjectID device) {
    std::string uid;
    GetString(device, Address(kAudioDevicePropertyDeviceUID), uid);
    return uid;
}

UInt32 ChannelCount(AudioObjectID device, AudioObjectPropertyScope scope) {
    auto address = Address(kAudioDevicePropertyStreamConfiguration, scope);
    UInt32 size = 0;
    if (PropertySize(device, address, size) != noErr || size == 0) return 0;
    std::vector<char> storage(size);
    auto* list = reinterpret_cast<AudioBufferList*>(storage.data());
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, list)
        != noErr) {
        return 0;
    }
    UInt32 channels = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i) {
        channels += list->mBuffers[i].mNumberChannels;
    }
    return channels;
}

void ListDevices() {
    printf("Core Audio devices:\n");
    for (AudioObjectID device : AllDevices()) {
        printf("  id %-6u in %3u out %3u  %-32s uid=%s\n", device,
               ChannelCount(device, kAudioObjectPropertyScopeInput),
               ChannelCount(device, kAudioObjectPropertyScopeOutput),
               DeviceName(device).c_str(), DeviceUID(device).c_str());
    }
}

AudioObjectID FindByUID(const std::string& uid) {
    for (AudioObjectID device : AllDevices()) {
        if (DeviceUID(device) == uid) return device;
    }
    return kAudioObjectUnknown;
}

AudioObjectID FindByName(const std::string& needle) {
    for (AudioObjectID device : AllDevices()) {
        if (DeviceName(device).find(needle) != std::string::npos) return device;
    }
    return kAudioObjectUnknown;
}

// ---------------------------------------------------------------------------
// Checks
// ---------------------------------------------------------------------------

void CheckIdentity(AudioObjectID device) {
    printf("\nIdentity and transport\n");
    std::string value;
    OSStatus status = GetString(device, Address(kAudioObjectPropertyName), value);
    Record("kAudioObjectPropertyName", status == noErr && !value.empty(),
           status == noErr ? value : OSStatusString(status));

    status = GetString(device, Address(kAudioObjectPropertyManufacturer), value);
    Record("kAudioObjectPropertyManufacturer", status == noErr && !value.empty(),
           status == noErr ? value : OSStatusString(status));

    status = GetString(device, Address(kAudioDevicePropertyDeviceUID), value);
    Record("kAudioDevicePropertyDeviceUID", status == noErr && !value.empty(),
           status == noErr ? value : OSStatusString(status));

    status = GetString(device, Address(kAudioDevicePropertyModelUID), value);
    Record("kAudioDevicePropertyModelUID", status == noErr && !value.empty(),
           status == noErr ? value : OSStatusString(status));

    UInt32 transport = 0;
    status = GetScalar(device, Address(kAudioDevicePropertyTransportType),
                       transport);
    char fourcc[5] = {0};
    UInt32 be = CFSwapInt32HostToBig(transport);
    memcpy(fourcc, &be, 4);
    Record("kAudioDevicePropertyTransportType", status == noErr,
           status == noErr ? std::string("'") + fourcc + "'"
                           : OSStatusString(status));

    UInt32 alive = 0;
    status = GetScalar(device, Address(kAudioDevicePropertyDeviceIsAlive), alive);
    Record("device is alive", status == noErr && alive != 0,
           status == noErr ? (alive ? "yes" : "no") : OSStatusString(status));

    UInt32 clockDomain = 0;
    status = GetScalar(device, Address(kAudioDevicePropertyClockDomain),
                       clockDomain);
    if (status == noErr) {
        Record("kAudioDevicePropertyClockDomain", true,
               std::to_string(clockDomain));
    } else {
        // Optional for a device that is its own clock master.
        Skip("kAudioDevicePropertyClockDomain", OSStatusString(status));
    }
}

void CheckTiming(AudioObjectID device) {
    printf("\nTiming properties\n");
    for (auto scope :
         {kAudioObjectPropertyScopeInput, kAudioObjectPropertyScopeOutput}) {
        const char* label =
            scope == kAudioObjectPropertyScopeInput ? "input" : "output";
        if (ChannelCount(device, scope) == 0) {
            Skip(std::string("latency (") + label + ")", "no channels in scope");
            continue;
        }
        UInt32 latency = 0;
        OSStatus status =
            GetScalar(device, Address(kAudioDevicePropertyLatency, scope),
                      latency);
        Record(std::string("kAudioDevicePropertyLatency (") + label + ")",
               status == noErr,
               status == noErr ? std::to_string(latency) + " frames"
                               : OSStatusString(status));

        UInt32 safety = 0;
        status = GetScalar(device, Address(kAudioDevicePropertySafetyOffset, scope),
                           safety);
        Record(std::string("kAudioDevicePropertySafetyOffset (") + label + ")",
               status == noErr,
               status == noErr ? std::to_string(safety) + " frames"
                               : OSStatusString(status));
    }

    UInt32 bufferFrames = 0;
    OSStatus status = GetScalar(
        device, Address(kAudioDevicePropertyBufferFrameSize), bufferFrames);
    Record("kAudioDevicePropertyBufferFrameSize", status == noErr && bufferFrames,
           status == noErr ? std::to_string(bufferFrames) + " frames"
                           : OSStatusString(status));

    AudioValueRange range{};
    status = GetScalar(device, Address(kAudioDevicePropertyBufferFrameSizeRange),
                       range);
    bool sane = status == noErr && range.mMinimum > 0
                && range.mMaximum >= range.mMinimum
                && bufferFrames >= range.mMinimum
                && bufferFrames <= range.mMaximum;
    char detail[128];
    snprintf(detail, sizeof(detail), "%.0f..%.0f frames, current %u",
             range.mMinimum, range.mMaximum, bufferFrames);
    Record("buffer frame size within range", sane,
           status == noErr ? detail : OSStatusString(status));
}

std::vector<double> AvailableRates(AudioObjectID device) {
    std::vector<AudioValueRange> ranges;
    GetArray(device, Address(kAudioDevicePropertyAvailableNominalSampleRates),
             ranges);
    std::vector<double> rates;
    for (const auto& range : ranges) {
        rates.push_back(range.mMinimum);
        if (range.mMaximum != range.mMinimum) rates.push_back(range.mMaximum);
    }
    return rates;
}

void CheckStreams(AudioObjectID device) {
    printf("\nStreams and formats\n");
    std::vector<AudioObjectID> streams;
    OSStatus status =
        GetArray(device, Address(kAudioDevicePropertyStreams), streams);
    Record("kAudioDevicePropertyStreams", status == noErr && !streams.empty(),
           status == noErr ? std::to_string(streams.size()) + " streams"
                           : OSStatusString(status));
    if (status != noErr) return;

    UInt32 inputs = ChannelCount(device, kAudioObjectPropertyScopeInput);
    UInt32 outputs = ChannelCount(device, kAudioObjectPropertyScopeOutput);
    Record("stream configuration", inputs + outputs > 0,
           std::to_string(inputs) + " in / " + std::to_string(outputs) + " out");

    for (AudioObjectID stream : streams) {
        UInt32 direction = 0;
        GetScalar(stream, Address(kAudioStreamPropertyDirection), direction);
        UInt32 startingChannel = 0;
        GetScalar(stream, Address(kAudioStreamPropertyStartingChannel),
                  startingChannel);
        const char* label = direction == 1 ? "input" : "output";

        AudioStreamBasicDescription format{};
        status = GetScalar(stream, Address(kAudioStreamPropertyVirtualFormat),
                           format);
        char detail[192];
        if (status == noErr) {
            snprintf(detail, sizeof(detail),
                     "%s ch%u: %.0f Hz, %u ch, %u bit, flags 0x%x", label,
                     startingChannel, format.mSampleRate,
                     format.mChannelsPerFrame, format.mBitsPerChannel,
                     format.mFormatFlags);
        }
        bool ok = status == noErr && format.mSampleRate > 0
                  && format.mChannelsPerFrame > 0
                  && format.mFormatID == kAudioFormatLinearPCM;
        Record(std::string("virtual format (stream ") + std::to_string(stream)
                   + ")",
               ok, status == noErr ? detail : OSStatusString(status));

        AudioStreamBasicDescription physical{};
        status = GetScalar(stream, Address(kAudioStreamPropertyPhysicalFormat),
                           physical);
        bool matches = status == noErr
                       && physical.mSampleRate == format.mSampleRate;
        Record(std::string("physical format tracks virtual (stream ")
                   + std::to_string(stream) + ")",
               matches,
               status == noErr
                   ? std::to_string(static_cast<long>(physical.mSampleRate))
                         + " Hz, " + std::to_string(physical.mBitsPerChannel)
                         + " bit"
                   : OSStatusString(status));

        std::vector<AudioStreamRangedDescription> available;
        status = GetArray(stream,
                          Address(kAudioStreamPropertyAvailablePhysicalFormats),
                          available);
        Record(std::string("available physical formats (stream ")
                   + std::to_string(stream) + ")",
               status == noErr && !available.empty(),
               status == noErr ? std::to_string(available.size()) + " formats"
                               : OSStatusString(status));
    }
}

bool WaitForRate(AudioObjectID device, double expected, int timeoutMs) {
    auto address = Address(kAudioDevicePropertyNominalSampleRate);
    for (int elapsed = 0; elapsed <= timeoutMs; elapsed += 20) {
        double current = 0.0;
        if (GetScalar(device, address, current) == noErr
            && std::fabs(current - expected) < 0.5) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

void CheckSampleRateNegotiation(AudioObjectID device) {
    printf("\nSample-rate negotiation\n");
    auto address = Address(kAudioDevicePropertyNominalSampleRate);
    double original = 0.0;
    OSStatus status = GetScalar(device, address, original);
    if (status != noErr) {
        Record("read nominal sample rate", false, OSStatusString(status));
        return;
    }
    Record("read nominal sample rate", true,
           std::to_string(static_cast<long>(original)) + " Hz");

    std::vector<double> rates = AvailableRates(device);
    Record("kAudioDevicePropertyAvailableNominalSampleRates", !rates.empty(),
           std::to_string(rates.size()) + " rates");

    for (double rate : rates) {
        status = AudioObjectSetPropertyData(device, &address, 0, nullptr,
                                            sizeof(rate), &rate);
        bool applied = status == noErr && WaitForRate(device, rate, 400);
        Record("set " + std::to_string(static_cast<long>(rate)) + " Hz", applied,
               status == noErr ? (applied ? "read back" : "not applied in 400 ms")
                               : OSStatusString(status));
    }

    status = AudioObjectSetPropertyData(device, &address, 0, nullptr,
                                        sizeof(original), &original);
    Record("restore original rate",
           status == noErr && WaitForRate(device, original, 400),
           std::to_string(static_cast<long>(original)) + " Hz");
}

// ---------------------------------------------------------------------------
// IOProc run
// ---------------------------------------------------------------------------

struct IOState {
    std::atomic<uint64_t> callbacks{0};
    std::atomic<uint64_t> frames{0};
    std::atomic<uint64_t> nonMonotonic{0};
    std::atomic<uint64_t> nonZeroInput{0};
    std::atomic<double> firstSampleTime{-1.0};
    std::atomic<double> lastSampleTime{-1.0};
};

OSStatus IOCallback(AudioObjectID /*device*/, const AudioTimeStamp* now,
                    const AudioBufferList* inputData,
                    const AudioTimeStamp* inputTime,
                    AudioBufferList* outputData,
                    const AudioTimeStamp* /*outputTime*/, void* clientData) {
    auto* state = static_cast<IOState*>(clientData);
    state->callbacks.fetch_add(1);

    double sampleTime = 0.0;
    if (inputTime && (inputTime->mFlags & kAudioTimeStampSampleTimeValid)) {
        sampleTime = inputTime->mSampleTime;
    } else if (now && (now->mFlags & kAudioTimeStampSampleTimeValid)) {
        sampleTime = now->mSampleTime;
    }
    double previous = state->lastSampleTime.load();
    if (previous >= 0.0 && sampleTime < previous) {
        state->nonMonotonic.fetch_add(1);
    }
    if (state->firstSampleTime.load() < 0.0) {
        state->firstSampleTime.store(sampleTime);
    }
    state->lastSampleTime.store(sampleTime);

    if (inputData) {
        for (UInt32 i = 0; i < inputData->mNumberBuffers; ++i) {
            const auto& buffer = inputData->mBuffers[i];
            UInt32 count = buffer.mDataByteSize / sizeof(float);
            if (i == 0) {
                state->frames.fetch_add(
                    buffer.mNumberChannels
                        ? count / buffer.mNumberChannels
                        : 0);
            }
            const auto* samples = static_cast<const float*>(buffer.mData);
            if (!samples) continue;
            for (UInt32 s = 0; s < count; ++s) {
                if (samples[s] != 0.0f) {
                    state->nonZeroInput.fetch_add(1);
                    break;
                }
            }
        }
    }
    if (outputData) {
        for (UInt32 i = 0; i < outputData->mNumberBuffers; ++i) {
            memset(outputData->mBuffers[i].mData, 0,
                   outputData->mBuffers[i].mDataByteSize);
        }
    }
    return noErr;
}

bool InputAccessBlocked(AudioObjectID device) {
    if (ChannelCount(device, kAudioObjectPropertyScopeInput) == 0) return false;
    return HALValidateMicrophoneAuthorization() != 1;
}

void CheckIO(AudioObjectID device, double seconds, bool forceIO) {
    printf("\nLive IOProc run (%.1f s)\n", seconds);
    if (InputAccessBlocked(device) && !forceIO) {
        Skip("live IOProc run",
             "no microphone access for this terminal; opening a device with "
             "input streams would block. Grant it in System Settings > Privacy "
             "& Security > Microphone, or pass --force-io.");
        return;
    }
    IOState state;
    AudioDeviceIOProcID procID = nullptr;
    OSStatus status =
        AudioDeviceCreateIOProcID(device, IOCallback, &state, &procID);
    if (status != noErr || procID == nullptr) {
        Record("AudioDeviceCreateIOProcID", false, OSStatusString(status));
        return;
    }
    Record("AudioDeviceCreateIOProcID", true, "created");

    double rate = 0.0;
    GetScalar(device, Address(kAudioDevicePropertyNominalSampleRate), rate);
    UInt32 bufferFrames = 0;
    GetScalar(device, Address(kAudioDevicePropertyBufferFrameSize), bufferFrames);

    status = AudioDeviceStart(device, procID);
    Record("AudioDeviceStart", status == noErr,
           status == noErr ? "started" : OSStatusString(status));
    if (status != noErr) {
        AudioDeviceDestroyIOProcID(device, procID);
        return;
    }

    std::this_thread::sleep_for(
        std::chrono::milliseconds(static_cast<int>(seconds * 1000)));

    UInt32 running = 0;
    GetScalar(device, Address(kAudioDevicePropertyDeviceIsRunning), running);
    Record("device reports running", running != 0, running ? "yes" : "no");

    status = AudioDeviceStop(device, procID);
    Record("AudioDeviceStop", status == noErr,
           status == noErr ? "stopped" : OSStatusString(status));
    AudioDeviceDestroyIOProcID(device, procID);

    uint64_t callbacks = state.callbacks.load();
    Record("IOProc was called", callbacks > 0,
           std::to_string(callbacks) + " callbacks");
    if (callbacks == 0) return;

    Record("sample time is monotonic", state.nonMonotonic.load() == 0,
           std::to_string(state.nonMonotonic.load()) + " backward steps");

    if (rate > 0.0 && bufferFrames > 0) {
        double expected = seconds * rate / bufferFrames;
        double ratio = static_cast<double>(callbacks) / expected;
        char detail[128];
        snprintf(detail, sizeof(detail),
                 "%llu observed vs %.0f expected (%.1f%%)",
                 static_cast<unsigned long long>(callbacks), expected,
                 100.0 * ratio);
        // A HAL device that keeps time calls back once per buffer; 15% is
        // enough slack for the start-up transient in a run of a few seconds.
        Record("callback rate matches buffer size",
               ratio > 0.85 && ratio < 1.15, detail);

        double advanced =
            state.lastSampleTime.load() - state.firstSampleTime.load();
        double expectedFrames = seconds * rate;
        char clockDetail[128];
        snprintf(clockDetail, sizeof(clockDetail),
                 "%.0f frames advanced vs %.0f expected", advanced,
                 expectedFrames);
        Record("device clock advances with wall clock",
               advanced > 0.8 * expectedFrames && advanced < 1.2 * expectedFrames,
               clockDetail);
    }

    if (ChannelCount(device, kAudioObjectPropertyScopeInput) > 0) {
        printf("  [INFO] input carried %s\n",
               state.nonZeroInput.load() ? "non-zero samples"
                                         : "silence (no stream received)");
    }
}

void PrintUsage() {
    printf(
        "Usage: HALValidate [options]\n"
        "  --uid <uid>       device UID to validate (default %s)\n"
        "  --name <text>     match a device by name substring instead\n"
        "  --id <n>          use this AudioObjectID directly\n"
        "  --list            list Core Audio devices and exit\n"
        "  --seconds <n>     IOProc run length, default 2\n"
        "  --skip-io         skip the live IOProc run\n"
        "  --force-io        run the IOProc even without microphone access\n"
        "  --skip-rates      skip sample-rate negotiation\n"
        "  --verbose         print every property read\n",
        kDefaultDeviceUID);
}

}  // namespace

int main(int argc, char** argv) {
    // Line buffering so a piped run still shows how far it got when a
    // device stalls mid-check.
    setvbuf(stdout, nullptr, _IOLBF, 0);

    std::string uid = kDefaultDeviceUID;
    std::string name;
    AudioObjectID explicitID = kAudioObjectUnknown;
    double seconds = 2.0;
    bool skipIO = false;
    bool skipRates = false;
    bool forceIO = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s needs a value\n", what);
                exit(2);
            }
            return argv[++i];
        };
        if (arg == "--uid") {
            uid = next("--uid");
        } else if (arg == "--name") {
            name = next("--name");
        } else if (arg == "--id") {
            explicitID = static_cast<AudioObjectID>(std::stoul(next("--id")));
        } else if (arg == "--seconds") {
            seconds = std::stod(next("--seconds"));
        } else if (arg == "--skip-io") {
            skipIO = true;
        } else if (arg == "--force-io") {
            forceIO = true;
        } else if (arg == "--skip-rates") {
            skipRates = true;
        } else if (arg == "--verbose") {
            g_verbose = true;
        } else if (arg == "--list") {
            ListDevices();
            return 0;
        } else if (arg == "--help" || arg == "-h") {
            PrintUsage();
            return 0;
        } else {
            fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            PrintUsage();
            return 2;
        }
    }

    AudioObjectID device = explicitID;
    if (device == kAudioObjectUnknown) {
        device = name.empty() ? FindByUID(uid) : FindByName(name);
    }
    if (device == kAudioObjectUnknown) {
        fprintf(stderr,
                "device not found (%s). The plugin has to be installed in\n"
                "/Library/Audio/Plug-Ins/HAL and coreaudiod restarted before\n"
                "this check can run. `--list` shows what Core Audio does see.\n",
                name.empty() ? uid.c_str() : name.c_str());
        return 1;
    }

    printf("Validating device %u: %s (uid %s)\n", device,
           DeviceName(device).c_str(), DeviceUID(device).c_str());

    CheckIdentity(device);
    CheckTiming(device);
    CheckStreams(device);
    if (!skipRates) {
        CheckSampleRateNegotiation(device);
    } else {
        printf("\nSample-rate negotiation: skipped\n");
    }
    if (!skipIO) {
        CheckIO(device, seconds, forceIO);
    } else {
        printf("\nLive IOProc run: skipped\n");
    }

    int passed = 0, failed = 0, skipped = 0;
    for (const auto& check : g_checks) {
        if (check.skipped) {
            ++skipped;
        } else if (check.passed) {
            ++passed;
        } else {
            ++failed;
        }
    }
    printf("\n%d passed, %d failed, %d skipped\n", passed, failed, skipped);
    if (failed) {
        printf("failed checks:\n");
        for (const auto& check : g_checks) {
            if (!check.passed && !check.skipped) {
                printf("  %s: %s\n", check.name.c_str(), check.detail.c_str());
            }
        }
    }
    return failed ? 1 : 0;
}
