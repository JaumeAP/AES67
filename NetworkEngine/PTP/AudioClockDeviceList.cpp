#include "AudioClockDeviceList.h"

#include <CoreFoundation/CoreFoundation.h>

#include <vector>

namespace AES67 {

namespace {

std::string cfStringToStd(CFStringRef ref) {
    if (!ref) return {};
    CFIndex length = CFStringGetLength(ref);
    CFIndex maxSize = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    std::vector<char> buf(static_cast<size_t>(maxSize));
    if (!CFStringGetCString(ref, buf.data(), maxSize, kCFStringEncodingUTF8)) return {};
    return std::string(buf.data());
}

bool hasOwnClockDomain(AudioDeviceID deviceID) {
    UInt32 clockDomain = 0;
    UInt32 size = sizeof(clockDomain);
    AudioObjectPropertyAddress addr{
        kAudioDevicePropertyClockDomain,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    // Devices that don't publish this property at all (some virtual/
    // software devices) fail here — treated the same as clockDomain == 0:
    // not a candidate reference.
    OSStatus status = AudioObjectGetPropertyData(deviceID, &addr, 0, nullptr, &size, &clockDomain);
    return status == noErr && clockDomain != 0;
}

std::string deviceName(AudioDeviceID deviceID) {
    CFStringRef name = nullptr;
    UInt32 size = sizeof(name);
    AudioObjectPropertyAddress addr{
        kAudioObjectPropertyName,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    if (AudioObjectGetPropertyData(deviceID, &addr, 0, nullptr, &size, &name) != noErr || !name) {
        return "(unnamed device)";
    }
    std::string result = cfStringToStd(name);
    CFRelease(name);
    return result;
}

std::string deviceUID(AudioDeviceID deviceID) {
    CFStringRef uid = nullptr;
    UInt32 size = sizeof(uid);
    AudioObjectPropertyAddress addr{
        kAudioDevicePropertyDeviceUID,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    if (AudioObjectGetPropertyData(deviceID, &addr, 0, nullptr, &size, &uid) != noErr || !uid) {
        return "";
    }
    std::string result = cfStringToStd(uid);
    CFRelease(uid);
    return result;
}

} // namespace

std::vector<AudioClockDeviceInfo> listClockCapableAudioDevices(AudioDeviceID excludeDeviceID) {
    std::vector<AudioClockDeviceInfo> result;

    AudioObjectPropertyAddress devicesAddr{
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    UInt32 dataSize = 0;
    OSStatus status = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &devicesAddr,
                                                       0, nullptr, &dataSize);
    if (status != noErr || dataSize == 0) return result;

    const size_t deviceCount = dataSize / sizeof(AudioDeviceID);
    std::vector<AudioDeviceID> devices(deviceCount);
    status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &devicesAddr, 0, nullptr,
                                         &dataSize, devices.data());
    if (status != noErr) return result;

    result.reserve(deviceCount);
    for (AudioDeviceID deviceID : devices) {
        if (deviceID == excludeDeviceID) continue;
        if (!hasOwnClockDomain(deviceID)) continue;

        AudioClockDeviceInfo info;
        info.deviceID = deviceID;
        info.name = deviceName(deviceID);
        info.uid = deviceUID(deviceID);
        result.push_back(std::move(info));
    }

    return result;
}

AudioDeviceID resolveAudioDeviceUID(const std::string& uid) {
    if (uid.empty()) return kAudioObjectUnknown;

    CFStringRef uidRef = CFStringCreateWithCString(kCFAllocatorDefault, uid.c_str(), kCFStringEncodingUTF8);
    if (!uidRef) return kAudioObjectUnknown;

    AudioDeviceID deviceID = kAudioObjectUnknown;
    AudioValueTranslation translation{};
    translation.mInputData = &uidRef;
    translation.mInputDataSize = sizeof(uidRef);
    translation.mOutputData = &deviceID;
    translation.mOutputDataSize = sizeof(deviceID);

    UInt32 size = sizeof(translation);
    AudioObjectPropertyAddress addr{
        kAudioHardwarePropertyDeviceForUID,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, &translation);

    CFRelease(uidRef);
    return deviceID; // stays kAudioObjectUnknown if the lookup failed or found nothing
}

} // namespace AES67
