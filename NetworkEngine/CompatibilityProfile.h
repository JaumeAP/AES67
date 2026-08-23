//
// CompatibilityProfile.h
// AES67 macOS Driver
// Which flavour of AoIP gear this driver is being pointed at, and the
// constraints that flavour imposes.
//
// Every profile here is AES67 underneath — see
// Docs/audio_over_ip_standards_landscape.md. What differs is what each one
// *restricts*: ST 2110-30 Level A permits only 48 kHz where AES67 permits
// three rates; Dante requires multicast inside 239.69.0.0/16 where AES67
// permits any; and so on. Rather than scatter those rules through
// StreamManager, each profile states its own and validates against them.
//
// A profile is a filter, never a capability grant: selecting one can only
// narrow what this driver accepts, never widen it beyond what the code
// actually implements.
//
#pragma once

#include "../Driver/SDPParser.h"

#include <cstdint>
#include <string>
#include <vector>

namespace AES67 {

enum class CompatibilityProfileKind {
    /// AES67's own mandatory configuration and nothing narrower. The
    /// default, and what the driver did before profiles existed.
    AES67,
    /// RAVENNA. Natively AES67; its own extras (Bonjour discovery, stream
    /// redundancy) aren't implemented here, so as a *constraint set* this
    /// is currently identical to AES67 — see profileNotes().
    RAVENNA,
    /// SMPTE ST 2110-30 Level A: 48 kHz only, 1 ms packets, RTP timestamp
    /// offset must be zero.
    ST2110_30,
};

struct CompatibilityProfile {
    CompatibilityProfileKind kind{CompatibilityProfileKind::AES67};

    /// Sample rates this profile accepts. Empty means "any rate the device
    /// supports" — not currently used by any profile, but the distinction
    /// matters if one is added that genuinely doesn't constrain rates.
    std::vector<double> allowedSampleRates;

    /// Packet times in milliseconds. A stream whose ptime isn't listed is
    /// rejected.
    std::vector<uint32_t> allowedPtimesMs;

    /// Encodings accepted, as they appear in SDP ("L16", "L24", "AM824").
    std::vector<std::string> allowedEncodings;

    /// Hard ceiling on channels in a single flow. AES67's own limit is 8,
    /// and no profile here raises it — StreamManager::createTxStreamFlows()
    /// splits anything wider regardless.
    uint16_t maxChannelsPerFlow{8};

    /// When set, connection addresses must fall inside this /16 (as
    /// "239.69" for Dante). Empty means any valid multicast address.
    std::string requiredMulticastPrefix;

    /// ST 2110-30 requires the RTP stream clock offset to be zero, where
    /// AES67 permits a random one. Informational here: RTPTransmitter
    /// already starts at zero unconditionally, so nothing needs enforcing —
    /// but the flag records *why* that zero must not be randomised.
    bool requiresZeroRtpTimestampOffset{false};

    /// Human-readable name for the UI.
    std::string displayName;

    /// Constraints this profile can't currently enforce, and things the
    /// real standard requires that this driver doesn't do. Shown in the UI
    /// so selecting a profile never reads as a conformance claim.
    std::string caveats;

    /// Rejects a stream that violates this profile. Returns true if
    /// acceptable; otherwise false with a reason in `errorOut` (if given).
    bool validate(const SDPSession& sdp, std::string* errorOut) const;

    static CompatibilityProfile forKind(CompatibilityProfileKind kind);

    /// All selectable profiles, in UI order.
    static std::vector<CompatibilityProfile> all();

    static std::string kindToString(CompatibilityProfileKind kind);
    static CompatibilityProfileKind kindFromString(const std::string& s);
};

// ============================================================================
// Persistence — the user's selection, shared with ManagerApp
// ============================================================================

class CompatibilityProfileManager {
public:
    CompatibilityProfileManager();
    ~CompatibilityProfileManager();

    /// Selection from disk, or AES67 (the unrestricted baseline) if no
    /// selection has been saved or the file is unreadable.
    CompatibilityProfileKind load();

    bool save(CompatibilityProfileKind kind);

    std::string getConfigPath() const;

private:
    std::vector<std::string> getConfigSearchPaths();
    std::string findExistingConfig();
    bool ensureConfigDirectoryExists();

    std::string configPath_;
    static constexpr const char* kDefaultConfigFile = "compatibility_profile.json";
};

} // namespace AES67
