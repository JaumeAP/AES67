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

/// Which direction THIS driver may use when talking to the gear a profile
/// describes. Always from our own point of view — ReceiveOnly means we may
/// receive from the remote device (add an RX stream), never transmit to it.
enum class ProfileDirection {
    Any,          ///< No direction restriction (AES67, RAVENNA, ST2110-30, Dante)
    ReceiveOnly,  ///< We may only receive — the remote device has no network input.
                  ///< CP850: it renders and sends via AES67, it doesn't accept it.
    TransmitOnly, ///< We may only transmit — the remote device has no network output.
                  ///< DAC3202: it converts digital in to analog out, nothing comes back.
};

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
    /// Dante in AES67 mode (Audinate). Requires multicast inside
    /// 239.69.0.0/16 — that's Dante's own requirement, not AES67's.
    Dante,
    /// Dolby Atmos Cinema Processor CP850 (and CP950A/IMS3000): sends AES67
    /// over Ethernet to a Dolby Atmos Connect Interface (DAC3202 profile,
    /// below). Cinema audio, so 48/96 kHz per the DCI spec rather than
    /// AES67's own three rates.
    CP850,
    /// Dolby Atmos Connect Interface DAC3202: the receiving end of the same
    /// link a CP850 sends — 32 analog outputs, i.e. up to 4 flows of 8
    /// channels. Same constraint set as CP850; they're two ends of one
    /// link, not two different networks.
    DAC3202,
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

    /// Which way THIS driver may talk to the described gear. See
    /// ProfileDirection. Checked by validate(sdp, isTransmit, ...).
    ProfileDirection direction{ProfileDirection::Any};

    /// Ceiling on TOTAL channels in the relevant direction while this
    /// profile is active — not per-flow, cumulative across every RX (or
    /// every TX) stream at once. 0 means unlimited. CP850: 64 (its own
    /// rendered feed count); DAC3202: 32 (its analog output count).
    /// Enforced by StreamManager::canAddStream(), which is the one place
    /// that can see every stream currently open — CompatibilityProfile
    /// itself only ever sees one SDPSession at a time and can't total
    /// anything on its own.
    uint32_t maxTotalChannels{0};

    /// When set, connection addresses must fall inside this /16 (as
    /// "239.69" for Dante). Empty means any valid multicast address.
    std::string requiredMulticastPrefix;

    /// ST 2110-30 requires the RTP stream clock offset to be zero, where
    /// AES67 permits a random one. Informational here: RTPTransmitter
    /// already starts at zero unconditionally, so nothing needs enforcing —
    /// but the flag records *why* that zero must not be randomised.
    bool requiresZeroRtpTimestampOffset{false};

    /// True when this profile pins the PTP domain to a single value the
    /// user can't change — AES67's own mandatory configuration is domain 0.
    /// When false, the domain field is offered to the user (ManagerApp's
    /// AddStreamView Stepper) rather than locked.
    bool domainIsFixed{false};

    /// Meaningful only when domainIsFixed — the one value streams must use.
    uint8_t fixedDomain{0};

    /// DSCP value this profile's real-world gear expects on the wire
    /// (e.g. 46 = EF), or -1 if the profile doesn't have a documented one.
    /// Informational only: NetworkUtils::setQoSTrafficClass() exists but
    /// has no caller anywhere in this driver, so nothing currently applies
    /// this. Recorded so it isn't lost, not because it's enforced.
    int recommendedDscp{-1};

    /// Human-readable name for the UI.
    std::string displayName;

    /// Constraints this profile can't currently enforce, and things the
    /// real standard requires that this driver doesn't do. Shown in the UI
    /// so selecting a profile never reads as a conformance claim.
    std::string caveats;

    /// Rejects a stream that violates this profile. `isTransmit` is true
    /// for a stream THIS driver would send (StreamManager::createTxStream),
    /// false for one it would receive (StreamManager::addStream) — needed
    /// to check `direction`; everything else here is direction-agnostic.
    /// Returns true if acceptable; otherwise false with a reason in
    /// `errorOut` (if given). Does NOT check maxTotalChannels — that needs
    /// to see every stream at once, which only StreamManager can.
    bool validate(const SDPSession& sdp, bool isTransmit, std::string* errorOut) const;

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
