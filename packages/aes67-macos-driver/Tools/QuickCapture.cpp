//
// QuickCapture.cpp
// Minimal Core Audio capture test — records from a specified audio device
// and reports whether non-zero samples were received.
//

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <atomic>
#include <thread>
#include <chrono>
#include <string>

static std::atomic<uint64_t> g_totalSamples{0};
static std::atomic<uint64_t> g_nonZeroSamples{0};
static std::atomic<double> g_peakLevel{0.0};

static OSStatus inputCallback(void* inRefCon,
                               AudioUnitRenderActionFlags* ioActionFlags,
                               const AudioTimeStamp* inTimeStamp,
                               UInt32 inBusNumber,
                               UInt32 inNumberFrames,
                               AudioBufferList* ioData) {
    AudioUnit inputUnit = *static_cast<AudioUnit*>(inRefCon);

    // Allocate buffer for capture
    AudioBufferList bufferList;
    bufferList.mNumberBuffers = 1;
    bufferList.mBuffers[0].mNumberChannels = 2;
    bufferList.mBuffers[0].mDataByteSize = inNumberFrames * 2 * sizeof(Float32);
    Float32 buffer[inNumberFrames * 2];
    bufferList.mBuffers[0].mData = buffer;

    OSStatus status = AudioUnitRender(inputUnit, ioActionFlags, inTimeStamp,
                                       inBusNumber, inNumberFrames, &bufferList);
    if (status != noErr) return status;

    // Analyze samples
    Float32* samples = (Float32*)bufferList.mBuffers[0].mData;
    size_t count = inNumberFrames * 2;

    double localPeak = 0.0;
    uint64_t localNonZero = 0;

    for (size_t i = 0; i < count; ++i) {
        double absVal = fabs(samples[i]);
        if (absVal > 0.0001) localNonZero++;
        if (absVal > localPeak) localPeak = absVal;
    }

    g_totalSamples.fetch_add(count, std::memory_order_relaxed);
    g_nonZeroSamples.fetch_add(localNonZero, std::memory_order_relaxed);

    double currentPeak = g_peakLevel.load(std::memory_order_relaxed);
    while (localPeak > currentPeak &&
           !g_peakLevel.compare_exchange_weak(currentPeak, localPeak,
                                               std::memory_order_relaxed)) {}

    return noErr;
}

static AudioDeviceID findDeviceByName(const char* targetName) {
    AudioObjectPropertyAddress prop;
    prop.mSelector = kAudioHardwarePropertyDevices;
    prop.mScope = kAudioObjectPropertyScopeGlobal;
    prop.mElement = kAudioObjectPropertyElementMain;

    UInt32 dataSize = 0;
    OSStatus status = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject,
                                                      &prop, 0, nullptr, &dataSize);
    if (status != noErr) return kAudioObjectUnknown;

    int deviceCount = dataSize / sizeof(AudioDeviceID);
    AudioDeviceID devices[deviceCount];
    status = AudioObjectGetPropertyData(kAudioObjectSystemObject,
                                         &prop, 0, nullptr, &dataSize, devices);
    if (status != noErr) return kAudioObjectUnknown;

    for (int i = 0; i < deviceCount; ++i) {
        CFStringRef name = nullptr;
        UInt32 nameSize = sizeof(name);
        AudioObjectPropertyAddress nameProp;
        nameProp.mSelector = kAudioObjectPropertyName;
        nameProp.mScope = kAudioObjectPropertyScopeGlobal;
        nameProp.mElement = kAudioObjectPropertyElementMain;

        status = AudioObjectGetPropertyData(devices[i], &nameProp,
                                             0, nullptr, &nameSize, &name);
        if (status == noErr && name) {
            char buf[256];
            CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
            CFRelease(name);

            if (strstr(buf, targetName)) {
                fprintf(stderr, "Found device: \"%s\" (ID: %u)\n", buf, devices[i]);
                return devices[i];
            }
        }
    }

    return kAudioObjectUnknown;
}

int main(int argc, char* argv[]) {
    int durationSec = 5;
    const char* deviceName = "AES67";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--duration" && i + 1 < argc) durationSec = atoi(argv[++i]);
        else if (arg == "--device" && i + 1 < argc) deviceName = argv[++i];
    }

    fprintf(stderr, "QuickCapture: looking for device containing \"%s\"...\n", deviceName);

    AudioDeviceID deviceID = findDeviceByName(deviceName);
    if (deviceID == kAudioObjectUnknown) {
        fprintf(stderr, "Error: device \"%s\" not found\n", deviceName);
        return 1;
    }

    // Create AUHAL input unit
    AudioComponentDescription desc;
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_HALOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    desc.componentFlags = 0;
    desc.componentFlagsMask = 0;

    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if (!comp) {
        fprintf(stderr, "Error: could not find HAL output component\n");
        return 1;
    }

    AudioUnit inputUnit;
    OSStatus status = AudioComponentInstanceNew(comp, &inputUnit);
    if (status != noErr) {
        fprintf(stderr, "Error: AudioComponentInstanceNew failed (%d)\n", (int)status);
        return 1;
    }

    // Enable input, disable output
    UInt32 enableIO = 1;
    status = AudioUnitSetProperty(inputUnit, kAudioOutputUnitProperty_EnableIO,
                                   kAudioUnitScope_Input, 1, &enableIO, sizeof(enableIO));
    if (status != noErr) {
        fprintf(stderr, "Error: enable input failed (%d)\n", (int)status);
        return 1;
    }
    enableIO = 0;
    status = AudioUnitSetProperty(inputUnit, kAudioOutputUnitProperty_EnableIO,
                                   kAudioUnitScope_Output, 0, &enableIO, sizeof(enableIO));
    if (status != noErr) {
        fprintf(stderr, "Error: disable output failed (%d)\n", (int)status);
        return 1;
    }

    // Set input device
    status = AudioUnitSetProperty(inputUnit, kAudioOutputUnitProperty_CurrentDevice,
                                   kAudioUnitScope_Global, 0, &deviceID, sizeof(deviceID));
    if (status != noErr) {
        fprintf(stderr, "Error: set device failed (%d)\n", (int)status);
        return 1;
    }

    // Set format: 48kHz stereo float
    AudioStreamBasicDescription format;
    memset(&format, 0, sizeof(format));
    format.mSampleRate = 48000;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    format.mBitsPerChannel = 32;
    format.mChannelsPerFrame = 2;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = 8;
    format.mBytesPerPacket = 8;

    status = AudioUnitSetProperty(inputUnit, kAudioUnitProperty_StreamFormat,
                                   kAudioUnitScope_Output, 1, &format, sizeof(format));
    if (status != noErr) {
        fprintf(stderr, "Error: set format failed (%d)\n", (int)status);
        return 1;
    }

    // Set input callback
    AURenderCallbackStruct callbackStruct;
    callbackStruct.inputProc = inputCallback;
    callbackStruct.inputProcRefCon = &inputUnit;

    status = AudioUnitSetProperty(inputUnit, kAudioOutputUnitProperty_SetInputCallback,
                                   kAudioUnitScope_Global, 0,
                                   &callbackStruct, sizeof(callbackStruct));
    if (status != noErr) {
        fprintf(stderr, "Error: set callback failed (%d)\n", (int)status);
        return 1;
    }

    // Initialize and start
    status = AudioUnitInitialize(inputUnit);
    if (status != noErr) {
        fprintf(stderr, "Error: AudioUnitInitialize failed (%d)\n", (int)status);
        return 1;
    }

    status = AudioOutputUnitStart(inputUnit);
    if (status != noErr) {
        fprintf(stderr, "Error: AudioOutputUnitStart failed (%d)\n", (int)status);
        return 1;
    }

    fprintf(stderr, "Recording from \"%s\" for %d seconds...\n", deviceName, durationSec);

    // Wait, printing status each second
    for (int i = 0; i < durationSec; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        uint64_t total = g_totalSamples.load();
        uint64_t nonZero = g_nonZeroSamples.load();
        double peak = g_peakLevel.load();
        double pctNonZero = total > 0 ? (100.0 * nonZero / total) : 0.0;
        fprintf(stderr, "  [%ds] samples=%llu  non-zero=%.1f%%  peak=%.4f\n",
                i + 1, total, pctNonZero, peak);
    }

    // Stop and cleanup
    AudioOutputUnitStop(inputUnit);
    AudioUnitUninitialize(inputUnit);
    AudioComponentInstanceDispose(inputUnit);

    // Final report
    uint64_t total = g_totalSamples.load();
    uint64_t nonZero = g_nonZeroSamples.load();
    double peak = g_peakLevel.load();

    fprintf(stderr, "\n========================================\n");
    if (nonZero > 0) {
        fprintf(stderr, "AUDIO DETECTED!\n");
        fprintf(stderr, "  Total samples: %llu\n", total);
        fprintf(stderr, "  Non-zero:      %llu (%.1f%%)\n", nonZero, 100.0 * nonZero / total);
        fprintf(stderr, "  Peak level:    %.4f (%.1f dBFS)\n", peak, 20.0 * log10(peak));
    } else {
        fprintf(stderr, "NO AUDIO (all samples were zero/silence)\n");
        fprintf(stderr, "  Total samples: %llu\n", total);
    }
    fprintf(stderr, "========================================\n");

    return (nonZero > 0) ? 0 : 1;
}
