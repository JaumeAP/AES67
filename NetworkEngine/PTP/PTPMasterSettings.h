//
// PTPMasterSettings.h
// AES67 macOS Driver
// Persisted choice of PTP clock source — the "internal, or lock to this
// other audio device" setting ManagerApp's UI writes and the driver reads
// at startup. Same search-path / directory convention as StreamConfig.h's
// StreamConfigManager, kept in its own small file since this is a
// driver-wide setting, not a per-stream one.
//
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace AES67 {

struct PTPMasterSettings {
    bool masterCapable{false}; // false: PTPDInterface stays slave-only, unchanged from before this feature existed

    /// Whether the driver runs a PTP clock at all — see
    /// StreamManager::setPTPEnabled(). False keeps the behaviour every
    /// build before this had: the PTP subsystem compiled and never
    /// started. Opt-in because starting it opens sockets and threads on a
    /// path verified against hardware without them.
    bool ptpEnabled{false};

    /// Whether to refuse audio until the clock has locked. Only meaningful
    /// alongside ptpEnabled. False by default for the blunt reason that on
    /// a system carrying audio today, turning it on can only take audio
    /// away.
    bool requireLock{false};

    // "internal" or "localAudioDevice" — mirrors PTPClockSourceKind
    // (PTPArbitrator.h) without this header needing to include CoreAudio.
    std::string clockSourceKind{"internal"};

    // Device UID (kAudioDevicePropertyDeviceUID), not AudioDeviceID: UIDs
    // are stable across reboots/replugs, AudioDeviceIDs aren't. Empty
    // unless clockSourceKind == "localAudioDevice".
    std::string lockToDeviceUID;
};

class PTPMasterSettingsManager {
public:
    PTPMasterSettingsManager();
    ~PTPMasterSettingsManager();

    /// Returns settings from disk, or PTPMasterSettings{} (masterCapable
    /// false — exactly the old slave-only behavior) if no settings file
    /// exists yet or it's unreadable.
    PTPMasterSettings load();

    bool save(const PTPMasterSettings& settings);

    std::string getConfigPath() const;

private:
    std::vector<std::string> getConfigSearchPaths();
    std::string findExistingConfig();
    bool ensureConfigDirectoryExists();

    std::string configPath_;
    static constexpr const char* kDefaultConfigFile = "ptp_master.json";
};

} // namespace AES67
