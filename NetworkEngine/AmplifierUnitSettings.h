//
// AmplifierUnitSettings.h
// AES67 macOS Driver
// Which physical amplifier/interface unit in a chain this driver is
// feeding — persisted, chosen from ManagerApp's main window, read by the
// driver at startup. Same search-path / directory convention as
// PTPMasterSettings.h and DeviceChannelSettings.h.
//
// Only meaningful for a profile whose CompatibilityProfile::maxUnits > 1
// (the Dolby Atmos Connect endpoints, DMA and DAC3202 — see that field).
// Dolby chains up to three of them per auditorium, each carrying the next
// consecutive block of channels, distinguished on the wire by the next
// block of source UDP ports rather than by address. Selecting unit 2
// therefore doesn't change what this driver renders — only which channel
// group the receiving end will accept it as, via
// StreamManager::setTxFlowPortOffset().
//
// For every other profile this is loaded but unused: StreamManager clamps
// the offset to zero when the active profile doesn't use per-flow source
// ports, so a stray value left over from switching away from a Dolby
// profile can't affect anything.
//
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace AES67 {

struct AmplifierUnitSettings {
    /// 1-based. 1 = the first (or only) unit — the driver's behavior
    /// before this setting existed, and the only meaningful value for any
    /// profile whose CompatibilityProfile::maxUnits is 1.
    uint32_t unitIndex{1};

    /// Highest value ManagerApp offers, matching every Dolby profile's own
    /// maxUnits. Anything outside 1..kMaxUnits falls back to 1.
    static constexpr uint32_t kMaxUnits = 3;

    bool isValid() const { return unitIndex >= 1 && unitIndex <= kMaxUnits; }
};

class AmplifierUnitSettingsManager {
public:
    AmplifierUnitSettingsManager();
    ~AmplifierUnitSettingsManager();

    /// Settings from disk, or AmplifierUnitSettings{} (unit 1) if the file
    /// is missing, unreadable, or holds an out-of-range index.
    AmplifierUnitSettings load();

    bool save(const AmplifierUnitSettings& settings);

    std::string getConfigPath() const;

private:
    std::vector<std::string> getConfigSearchPaths();
    std::string findExistingConfig();
    bool ensureConfigDirectoryExists();

    std::string configPath_;
    static constexpr const char* kDefaultConfigFile = "amplifier_unit.json";
};

} // namespace AES67
