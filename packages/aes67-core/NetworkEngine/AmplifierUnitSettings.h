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

/// Grouped with the amplifier unit only because both are small
/// installation-level knobs read at startup; they are otherwise unrelated.
/// See StreamManager::setPlayoutDelaySamples().
struct PlayoutDelaySettings {
    uint32_t samples{0}; // 0 = the receiver's own default cushion
    static constexpr uint32_t kMaxSamples = 4800; // 100 ms at 48 kHz — beyond this it's a fault, not jitter
    bool isValid() const { return samples <= kMaxSamples; }
};

struct AmplifierUnitSettings {
    /// 1-based. 1 = the first (or only) unit — the driver's behavior
    /// before this setting existed, and the only meaningful value for any
    /// profile whose CompatibilityProfile::maxUnits is 1.
    uint32_t unitIndex{1};

    /// Highest value ManagerApp offers, matching every Dolby profile's own
    /// maxUnits. Anything outside 1..kMaxUnits falls back to 1.
    static constexpr uint32_t kMaxUnits = 3;

    /// One Atmos Connect flow carries at most this many channels (the
    /// standard's per-flow cap); the source-port stepping is per flow.
    static constexpr uint32_t kChannelsPerFlow = 8;

    /// Channel count of each unit in the chain, unit 1 first. Units in a
    /// chain need NOT be the same model: a 16-, 24- and 32-channel unit can
    /// sit in one chain, and each takes a DIFFERENT number of 8-channel flows
    /// (2, 3, 4) and therefore a different block of source ports. This records
    /// those per-unit sizes so the source-port offset for the selected unit is
    /// the real sum of the preceding units' flows — not (unitIndex-1) times
    /// this driver's own width, which only holds when every unit is identical.
    /// Empty, or a 0 entry, means "unknown — assume same width as this driver's
    /// output" (the old uniform behaviour), so an unset chain changes nothing.
    std::vector<uint32_t> chainUnitChannels;

    bool isValid() const { return unitIndex >= 1 && unitIndex <= kMaxUnits; }

    /// Source-port flow offset for `unitIndex`: the number of 8-channel flows
    /// the units BEFORE it occupy. Each preceding unit contributes
    /// ceil(channels / kChannelsPerFlow) flows, taken from chainUnitChannels
    /// when known and from `fallbackChannels` (this driver's own output count)
    /// otherwise. Pure and static so it is unit-tested on its own.
    static uint32_t flowOffsetForUnit(const std::vector<uint32_t>& chainUnitChannels,
                                      uint32_t fallbackChannels, uint32_t unitIndex) {
        uint32_t offset = 0;
        for (uint32_t i = 0; i + 1 < unitIndex; ++i) { // units 1..unitIndex-1 (0-based i)
            const uint32_t ch = (i < chainUnitChannels.size() && chainUnitChannels[i] > 0)
                                    ? chainUnitChannels[i]
                                    : fallbackChannels;
            offset += (ch + kChannelsPerFlow - 1) / kChannelsPerFlow;
        }
        return offset;
    }
};

class AmplifierUnitSettingsManager {
public:
    AmplifierUnitSettingsManager();
    ~AmplifierUnitSettingsManager();

    /// Settings from disk, or AmplifierUnitSettings{} (unit 1) if the file
    /// is missing, unreadable, or holds an out-of-range index.
    AmplifierUnitSettings load();

    bool save(const AmplifierUnitSettings& settings);

    /// Playout delay, from the same file. Zero if absent or out of range.
    PlayoutDelaySettings loadPlayoutDelay();
    bool savePlayoutDelay(const PlayoutDelaySettings& settings);

    std::string getConfigPath() const;

private:
    std::vector<std::string> getConfigSearchPaths();
    std::string findExistingConfig();
    bool ensureConfigDirectoryExists();

    std::string configPath_;
    static constexpr const char* kDefaultConfigFile = "amplifier_unit.json";
};

} // namespace AES67
