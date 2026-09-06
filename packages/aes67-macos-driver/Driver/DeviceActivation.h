//
// DeviceActivation.h
// AES67 macOS Driver
// Whether this plug-in publishes its audio device to Core Audio at all.
//
// Installed and active are two different things. The driver bundle sitting in
// /Library/Audio/Plug-Ins/HAL is what "installed" means; whether the AES67
// device then appears in the list every application picks from is this
// setting, which ManagerApp writes and PlugInMain reads once, while Core
// Audio is constructing the plug-in.
//
// Deactivating therefore does not remove anything: the bundle stays, the
// plug-in still loads, and it registers no device. Which is the point -- the
// settings the driver only reads at startup can be edited while nothing is
// using it.
//
// Same directory as PTPMasterSettings and the rest, but not the same search
// order: $AES67_DEVICE_ACTIVATION_PATH, then /Library/Application Support/
// AES67Driver/device_active.json, and only then the copy under a home
// directory. The Manager app writes this one to /Library through an
// administrator prompt, because the plug-in is constructed inside coreaudiod
// and its HOME is not the logged-in user's; a file left under some home
// directory must not be what decides whether the device appears.
//
// This lives in the driver package rather than the core because publishing a
// device is a Core Audio HAL concept, not a platform-free one.
//
#pragma once

#include <string>
#include <vector>

namespace AES67 {

struct DeviceActivation {
    /// Whether to register the device with the plug-in.
    ///
    /// True by default, and that default is load-bearing: a driver installed
    /// before this setting existed has no file to read, and it must keep
    /// publishing its device rather than silently disappear.
    bool active{true};
};

class DeviceActivationManager {
public:
    DeviceActivationManager();
    ~DeviceActivationManager();

    /// Returns the setting from disk, or DeviceActivation{} (active) when no
    /// file exists yet or it cannot be read or parsed.
    DeviceActivation load();

    bool save(const DeviceActivation& activation);

    std::string getConfigPath() const;

private:
    std::vector<std::string> getConfigSearchPaths();
    std::string findExistingConfig();
    bool ensureConfigDirectoryExists();

    std::string configPath_;
    static constexpr const char* kDefaultConfigFile = "device_active.json";
};

} // namespace AES67
