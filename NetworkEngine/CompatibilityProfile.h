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

/// Which PTP role THIS driver must take when talking to the gear a profile
/// describes. Always from our own point of view, same as ProfileDirection.
/// Unlike ProfileDirection this isn't enforced by StreamManager — the PTP
/// arbitrator (PTPArbitrator/PTPDInterface) isn't wired into the real
/// driver path yet (see NetworkEngine/PTP/PTPArbitrator.h), so today this is
/// consulted by ManagerApp only, to lock the "Act as PTP master" toggle
/// (PTPDiagnosticView) to the right value and grey it out.
enum class PTPRoleConstraint {
    Any,           ///< No restriction — BMCA decides (AES67, RAVENNA, ST2110-30, Dante).
    ForcedSlave,   ///< This driver must always be PTP slave under this profile.
                   ///< CP850.
    ForcedMaster,  ///< This driver must always be PTP master under this profile.
                   ///< DAC3202.
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
    /// Dolby Atmos Cinema Processor CP850 (and IMS3000): sends AES67
    /// over Ethernet to a Dolby Atmos Connect Interface (DAC3202 profile,
    /// below). Cinema audio, so 48/96 kHz per the DCI spec rather than
    /// AES67's own three rates.
    CP850,
    /// Dolby Cinema Processor CP950 / Atmos Cinema Processor CP950A — the
    /// current-generation replacement line for CP850 (its own manual says
    /// so explicitly). Same role as CP850 from this driver's point of
    /// view: it renders and sends, this driver only ever receives.
    /// CP950 outputs up to 16 channels over Atmos Connect, CP950A up to
    /// 64 — covered as one profile the same way DMA covers its whole
    /// model range, since it's the same protocol either way: pick the
    /// real unit's channel count with ManagerApp's Input selector rather
    /// than picking a separate profile per model.
    CP950,
    /// Dolby Atmos Connect Interface DAC3202: the receiving end of the same
    /// link a CP850/CP950/CP950A sends — 32 analog outputs, i.e. up to 4
    /// flows of 8 channels. Same constraint set as CP850; they're two ends
    /// of one link, not two different networks.
    DAC3202,
    /// Dolby Multichannel Amplifier — like DAC3202, another downstream
    /// endpoint on a Dolby Atmos Connect link; this driver plays the
    /// cinema-processor role sending to it, so transmit-only and always PTP
    /// master, confirmed (not assumed) by its manual
    /// (Docs/references/dolby_multichannel_amplifier_manual.pdf): the
    /// amplifier's own PTP clock priority is fixed at 255 (lowest) and it
    /// is documented as "strictly a downstream device" that never
    /// originates timing. Its Atmos Connect port scheme also differs from
    /// this driver's own flow splitter — see CompatibilityProfile.cpp's DMA
    /// case for specifics. Covers every real model (DMA16301/16302,
    /// DMA24300/24302, DMA32300/32301) as one profile: unlike CP850/
    /// DAC3202, the output channel count isn't fixed here — it's whatever
    /// the user picks with ManagerApp's Output channel-count selector, up
    /// to this profile's own maxTotalChannels ceiling.
    DMA,
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

    /// True for the Dolby Atmos Connect wire scheme, confirmed by two
    /// independent manuals — the DMA's own
    /// (Docs/references/dolby_multichannel_amplifier_manual.pdf, §3.2.4)
    /// and, from the sending side, the CP950/CP950A's
    /// (Docs/references/dolby_cp950_cp950a_manual.pdf, "RTP source and RTP
    /// destination UDP ports", same port table): one multicast address and
    /// a FIXED RTP destination port shared by every flow, with each flow's
    /// SOURCE port stepped instead (base port +1, +2, +3, ... — e.g. 6517
    /// fixed destination, 6518/6519/6520/... source per 8-channel block).
    /// The default (false) is every other profile's scheme: one multicast
    /// address PER flow (last octet advanced), shared port, kernel-assigned
    /// ephemeral source port — the AES67/Dante convention. Set for DMA and
    /// DAC3202 — the CP950/CP950A manual's port table and shared default
    /// multicast address (see recommendedMulticastAddress) are both
    /// documented as applying to "a Dolby Multichannel Amplifier, Dolby
    /// DAC3202, or another compatible device" equally, so this is no
    /// longer DMA-only speculation extended to DAC3202, it's the confirmed
    /// shared scheme. CP850/CP950/CP950A don't set it: they're always the
    /// SENDING side under this driver's profiles (ReceiveOnly), so this
    /// driver never runs createTxStreamFlows() under them regardless.
    /// Read by StreamManager::createTxStreamFlows().
    bool useFixedMulticastWithPerFlowSourcePort{false};

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

    /// PTP domain this profile's real-world gear defaults to when the
    /// domain isn't fixed (domainIsFixed == false) — e.g. 109 for the Dolby
    /// Multichannel Amplifier (DMA) family, confirmed by its manual
    /// (Docs/references/dolby_multichannel_amplifier_manual.pdf, §3.2.4
    /// "PTP Domain Number"). -1 means no documented recommendation; the
    /// field is meaningless when domainIsFixed is true (fixedDomain is the
    /// only value that matters then). Informational only, same as
    /// recommendedDscp below — not applied by AddStreamView's Stepper,
    /// which still starts free at 0 regardless.
    int recommendedPtpDomain{-1};

    /// Destination multicast address this profile's real-world gear
    /// defaults to — confirmed by the CP950/CP950A manual
    /// (Docs/references/dolby_cp950_cp950a_manual.pdf, "Destination
    /// multicast IP"): 239.81.83.67, explicitly documented as shared by
    /// "the Dolby CP950/CP950A, Dolby Multichannel Amplifier, and Dolby
    /// DAC3202". Empty means no documented default. Informational only,
    /// same as recommendedPtpDomain — nothing here picks a stream's
    /// connection address for the user.
    std::string recommendedMulticastAddress;

    /// Which PTP role this driver must take while this profile is active.
    /// See PTPRoleConstraint. Not enforced here — informational for
    /// ManagerApp, which locks its "Act as PTP master" toggle accordingly.
    PTPRoleConstraint ptpRole{PTPRoleConstraint::Any};

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
