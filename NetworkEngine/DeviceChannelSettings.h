//
// DeviceChannelSettings.h
// AES67 macOS Driver
// How many channels the device exposes to Core Audio — persisted, chosen
// from ManagerApp's main window, read by the driver at startup.
//
// Deliberately NOT a runtime-resizable thing: the ring buffers behind the
// real-time path stay a fixed std::array<..., kMaxDeviceChannels> (128),
// allocated once, never touched again. Selecting fewer channels changes only
// what the device *advertises* in its stream format — the RT path, the one
// part of this driver verified against real hardware, is not restructured.
// Changing the selection therefore takes effect when Core Audio next starts
// the driver, not while audio is running.
//
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace AES67 {

struct DeviceChannelSettings {
    /// Channels the device exposes, in each direction (input and output
    /// alike). One of kAllowedChannelCounts; anything else is rejected by
    /// isValid() and falls back to the default.
    uint32_t channelCount{128};

    /// Auxiliary channel pair. When enabled, the device exposes one extra
    /// group of 8 on top of channelCount, of which the first 2 are the
    /// auxiliary pair and the remaining 6 are reserved padding.
    ///
    /// Why a whole group of 8 for 2 channels: the device works internally in
    /// groups of 8, and 8+2 = 10 is not a multiple of 8. Rounding the aux
    /// pair up to its own group keeps every total 8-aligned (8->16, 128->136)
    /// rather than breaking the invariant for the sake of 6 channels.
    bool auxChannelEnabled{false};

    /// Channels in the auxiliary group that actually carry audio.
    static constexpr uint32_t kAuxChannelCount = 2;

    /// Everything is a multiple of this — see auxChannelEnabled.
    static constexpr uint32_t kChannelGroupSize = 8;

    /// Hard ceiling: the compile-time size of the RT ring buffer arrays
    /// (AES67Device::kNumChannels / RTSafeStreamInterface::kNumChannels).
    /// totalChannelCount() never exceeds this.
    static constexpr uint32_t kMaxDeviceChannels = 128;

    static const std::vector<uint32_t>& allowedChannelCounts();

    /// channelCount plus the auxiliary group, clamped to kMaxDeviceChannels.
    ///
    /// The clamp matters at the top of the range: 128 + 8 = 136 would
    /// overrun the fixed RT buffers, so with 128 selected the auxiliary
    /// group has nowhere to go and the total stays 128. isValid() rejects
    /// that combination outright so the UI can say so rather than silently
    /// dropping the auxiliary channels.
    uint32_t totalChannelCount() const;

    /// True if channelCount is one of the allowed values and the auxiliary
    /// group (if enabled) fits within kMaxDeviceChannels.
    bool isValid() const;
};

class DeviceChannelSettingsManager {
public:
    DeviceChannelSettingsManager();
    ~DeviceChannelSettingsManager();

    /// Settings from disk, or defaults (128 channels, no auxiliary — the
    /// driver's behavior before this setting existed) if the file is
    /// missing, unreadable, or holds an invalid combination.
    DeviceChannelSettings load();

    bool save(const DeviceChannelSettings& settings);

    std::string getConfigPath() const;

private:
    std::vector<std::string> getConfigSearchPaths();
    std::string findExistingConfig();
    bool ensureConfigDirectoryExists();

    std::string configPath_;
    static constexpr const char* kDefaultConfigFile = "device_channels.json";
};

} // namespace AES67
