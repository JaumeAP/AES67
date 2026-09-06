//
// DeviceChannelSettings.h
// AES67 macOS Driver
// How many channels the device makes USABLE, per direction — persisted,
// chosen from ManagerApp's main window as two independent selectors (input
// / output), read by the driver at startup.
//
// Two selectors, not one, because direction isn't symmetric once a
// compatibility profile restricts it: CP850 (Profiles/CompatibilityProfile.h)
// is receive-only from this driver's own point of view, DAC3202 is
// transmit-only. The input selector is meaningless under a transmit-only
// profile and vice versa — ManagerApp disables whichever side the active
// profile rules out, rather than offering a control that would just be
// rejected on submit.
//
// Deliberately NOT a runtime-resizable thing: the ring buffers behind the
// real-time path stay a fixed std::array<..., kMaxDeviceChannels> (128) in
// each direction, allocated once, never touched again. Selecting fewer
// channels changes only how many StreamManager will actually hand out to
// streams (StreamManager::canAddStream) — the RT path, the one part of this
// driver verified against real hardware, is not restructured. Changing the
// selection therefore takes effect when Core Audio next starts the driver,
// not while audio is running.
//
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace AES67 {

/// One direction's half of the setting — same shape for input and output,
/// deliberately: the group-of-8 rule applies identically to both.
struct DeviceChannelSelection {
    /// One of DeviceChannelSettings::allowedChannelCounts(); anything else
    /// is rejected by isValid() and falls back to the default.
    uint32_t channelCount{128};

    /// Auxiliary channel pair. When enabled, this direction gets one extra
    /// group of 8 on top of channelCount, of which the first 2 are the
    /// auxiliary pair and the remaining 6 are reserved padding.
    ///
    /// Why a whole group of 8 for 2 channels: the device works internally in
    /// groups of 8, and 8+2 = 10 is not a multiple of 8. Rounding the aux
    /// pair up to its own group keeps every total 8-aligned (8->16, 128->136)
    /// rather than breaking the invariant for the sake of 6 channels.
    bool auxChannelEnabled{false};

    /// channelCount plus the auxiliary group, clamped to
    /// DeviceChannelSettings::kMaxDeviceChannels.
    ///
    /// The clamp matters at the top of the range: 128 + 8 = 136 would
    /// overrun the fixed RT buffers, so with 128 selected the auxiliary
    /// group has nowhere to go and the total stays 128. isValid() rejects
    /// that combination outright so the UI can say so rather than silently
    /// dropping the auxiliary channels.
    uint32_t totalChannelCount() const;

    bool isValid() const;
};

struct DeviceChannelSettings {
    DeviceChannelSelection rx; ///< Input: Network -> Core Audio
    DeviceChannelSelection tx; ///< Output: Core Audio -> Network

    /// Channels in the auxiliary group that actually carry audio.
    static constexpr uint32_t kAuxChannelCount = 2;

    /// Everything is a multiple of this — see DeviceChannelSelection::auxChannelEnabled.
    static constexpr uint32_t kChannelGroupSize = 8;

    /// Hard ceiling: the compile-time size of the RT ring buffer arrays
    /// (AES67Device::kNumChannels / RTSafeStreamInterface::kNumChannels),
    /// in EACH direction independently.
    static constexpr uint32_t kMaxDeviceChannels = 128;

    /// Selectable usable-channel totals: every group of 8 up to
    /// kMaxDeviceChannels (8, 16, 24, ... 128). A cap on how many of the
    /// fixed 128 buffer channels are assigned, not a change to the buffers.
    static const std::vector<uint32_t>& allowedChannelCounts();

    /// True if both rx and tx are individually valid.
    bool isValid() const;
};

class DeviceChannelSettingsManager {
public:
    DeviceChannelSettingsManager();
    ~DeviceChannelSettingsManager();

    /// Settings from disk, or defaults (128 channels each direction, no
    /// auxiliary — the driver's behavior before this setting existed) if
    /// the file is missing, unreadable, or holds an invalid combination.
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
