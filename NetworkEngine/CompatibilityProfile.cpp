#include "CompatibilityProfile.h"
#include "../Driver/DebugLog.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <pwd.h>
#include <regex>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace AES67 {

// ============================================================================
// Profile definitions
// ============================================================================

CompatibilityProfile CompatibilityProfile::forKind(CompatibilityProfileKind kind) {
    CompatibilityProfile p;
    p.kind = kind;

    switch (kind) {
    case CompatibilityProfileKind::AES67:
        p.displayName = "AES67";
        // AES67's mandatory configuration: 1-8 channels, 16/24-bit,
        // 44.1/48/96 kHz, 1 ms packets.
        p.allowedSampleRates = {44100.0, 48000.0, 96000.0};
        p.allowedPtimesUs = {1000}; // 1 ms
        p.allowedEncodings = {"L16", "L24"};
        p.maxChannelsPerFlow = 8;
        p.requiresZeroRtpTimestampOffset = false; // AES67 permits a random offset
        // AES67's mandatory configuration is PTP domain 0 — not a default
        // to override, part of what "AES67" means here. See
        // PTPSlaveConfig's own "PTP domain 0 (default), per AES67" comment.
        p.domainIsFixed = true;
        p.fixedDomain = 0;
        p.caveats =
            "Baseline. Accepts the three sample rates AES67 names; the device "
            "itself declares more (up to 384 kHz), which other AES67 gear may "
            "refuse. PTP domain fixed at 0.";
        break;

    case CompatibilityProfileKind::RAVENNA:
        p.displayName = "RAVENNA";
        // RAVENNA is a SUPERSET of AES67, so this profile must accept, not
        // narrow, what RAVENNA gear can send — otherwise a legitimate
        // RAVENNA stream (a high sample rate, a sub-millisecond packet time)
        // would be rejected on receive and the profile would be "compatible"
        // in name only. So:
        //  - the full RAVENNA sample-rate set, not AES67's three;
        //  - NO packet-time restriction at all (empty = validate() accepts
        //    any ptime), because RAVENNA frame sizes run 1-192 samples, a
        //    continuum of durations no fixed list could enumerate. Our own
        //    transmitter still emits 1 ms (a valid RAVENNA ptime); the empty
        //    set only widens what we ACCEPT, it doesn't make us send anything
        //    new.
        // Encodings stay L16/L24: those are what PCMCodec can actually
        // decode. RAVENNA also defines L32, but accepting an SDP we can't
        // decode would be a false claim, so it is deliberately excluded.
        p.allowedSampleRates = {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0};
        p.allowedPtimesUs = {}; // empty = accept any packet time (RAVENNA is unrestricted here)
        p.allowedEncodings = {"L16", "L24"};
        p.maxChannelsPerFlow = 8;
        p.requiresZeroRtpTimestampOffset = false;
        p.caveats =
            "A true AES67 superset on receive: accepts RAVENNA's full sample-"
            "rate set (44.1-192 kHz) and any packet time, so a RAVENNA source "
            "is not rejected for using a rate or ptime AES67 doesn't name. "
            "Two honest edges remain, both receiver-architecture limits, not "
            "RAVENNA ones: a single stream is still capped at 8 channels per "
            "flow (wider RAVENNA streams must be split), and only L16/L24 are "
            "decoded (RAVENNA's L32 is not). Transmit still emits 1 ms L24. "
            "RAVENNA's Bonjour discovery and stream redundancy are not "
            "implemented.";
        break;

    case CompatibilityProfileKind::ST2110_30:
        p.displayName = "SMPTE ST 2110-30 (Level A)";
        // Level A: 48 kHz only, 1 ms packets, 1-8 channels, 16/24-bit.
        // See Docs/st2110_30_vs_aes67.md.
        p.allowedSampleRates = {48000.0};
        p.allowedPtimesUs = {1000}; // 1 ms
        p.allowedEncodings = {"L16", "L24"}; // AM824 is ST 2110-31, not -30
        p.maxChannelsPerFlow = 8;
        p.requiresZeroRtpTimestampOffset = true;
        p.caveats =
            "The mandatory level, and the safe common ground: gear claiming "
            "any higher level must support this one too. 48 kHz, 1 ms "
            "packets, up to 8 channels per stream. Pick Level B instead for "
            "125 us packets. ST 2110-30 also requires stricter PTP than "
            "AES67, and this driver's PTP has never been verified against a "
            "real grandmaster. Selecting this profile enforces the "
            "parameters it can check; it is not a conformance claim.";
        break;

    case CompatibilityProfileKind::ST2110_30_LevelB:
        p.displayName = "SMPTE ST 2110-30 (Level B)";
        // Level B is Level A at 125 us instead of 1 ms — same 48 kHz, same
        // 16/24-bit, same 1-8 channels per stream. Emitting 125 us packets
        // is possible as of the commit that moved packet time to
        // microseconds; before that this profile could not have been
        // honoured on transmit at all.
        //
        // Levels C, AX, BX and CX are deliberately absent:
        //  - C is Level B with up to 64 channels in ONE stream, which this
        //    driver can't do — StreamChannelMapper::kMaxChannelsPerFlow
        //    caps a flow at 8 and the flow splitter divides anything wider.
        //    Offering it would be a claim this driver can't honour.
        //  - AX/BX/CX are the 96 kHz variants with the channel counts
        //    halved (4, 4, 32). Supportable in principle; not added
        //    speculatively, since nothing has asked for them.
        p.allowedSampleRates = {48000.0};
        p.allowedPtimesUs = {125};
        p.allowedEncodings = {"L16", "L24"}; // AM824 is ST 2110-31, not -30
        p.maxChannelsPerFlow = 8;
        p.requiresZeroRtpTimestampOffset = true;
        p.caveats =
            "Level A's constraints at a 125 us packet time: 48 kHz, up to 8 "
            "channels per stream. Only choose this if the receiving gear "
            "actually claims Level B — a Level A device must not be sent "
            "125 us packets, and Level A is what everything supports. This "
            "driver's transmitter emits whatever packet time the stream "
            "asks for, so 125 us is reachable, but it has never been tested "
            "against real Level B gear. Levels C (64 channels in one "
            "stream) and AX/BX/CX (96 kHz) are not offered — see the code "
            "comment for why. Same PTP caveat as Level A: ST 2110-30 "
            "requires stricter PTP than AES67 and this driver's has never "
            "been verified against a real grandmaster. Not a conformance "
            "claim.";
        break;

    case CompatibilityProfileKind::Dante:
        p.displayName = "Dante (AES67 mode)";
        // Source: Audinate's own Dante Controller "AES67 Config"
        // documentation. AES67 mode is NARROWER than AES67's own baseline
        // in three ways this profile previously got wrong by assuming
        // Dante simply inherited that baseline:
        //  - 48 kHz only. Dante devices run 44.1/96/... natively, but an
        //    AES67 flow out of one is 48 kHz, full stop.
        //  - L24 only. "AES67 flows generated by Dante devices must use 24
        //    bit linear encoding" — L16 is part of AES67's own baseline but
        //    not something a Dante device will produce or accept here.
        //  - PTPv2 domain fixed at 0. Dante's *native* PTPv1 clocking has
        //    its own domain concept, which is what the old "domains 0-127"
        //    comment here confused it with; AES67 mode itself is documented
        //    as a fixed domain 0, and consequently can only be enabled for
        //    one domain at a time.
        p.allowedSampleRates = {48000.0};
        p.allowedPtimesUs = {1000}; // 1 ms = 48 samples per channel per frame, per Audinate
        p.allowedEncodings = {"L24"};
        p.maxChannelsPerFlow = 8;
        p.requiresZeroRtpTimestampOffset = false;
        p.domainIsFixed = true;
        p.fixedDomain = 0;
        // Dante marks audio EF/46 (and PTP CS7/56 — a documented conflict
        // with 'standard' AES67 implementations, which mark PTP 46
        // instead). Informational only, like every recommendedDscp here.
        p.recommendedDscp = 46;
        // 239.69 is Dante's factory-default multicast prefix, and Dante
        // Controller can change it (the documented range is 239.nnn/16).
        // Kept as a hard requirement anyway, per explicit instruction: a
        // profile is a filter, and this catches the common
        // misconfiguration. A site that has deliberately moved its prefix
        // should select the AES67 baseline profile instead of this one.
        p.requiredMulticastPrefix = "239.69";
        p.caveats =
            "Requires the Dante device to have AES67 mode explicitly enabled "
            "— this driver can't do that remotely, it's a setting on the "
            "Dante hardware itself (Dante Controller). Dante natively "
            "syncs with PTPv1; AES67 mode is what switches it to PTPv2, "
            "which is what this driver speaks, on a fixed domain 0. "
            "AES67 mode is narrower than AES67 itself: 48 kHz only "
            "(whatever the device runs natively), L24 only, 1 ms packets, "
            "port 5004. Enforces the 239.69.0.0/16 multicast range — that "
            "prefix is Dante's factory default and is configurable in Dante "
            "Controller, so a site that has moved it should use the AES67 "
            "baseline profile instead. Dante marks audio DSCP EF/46 and PTP "
            "CS7/56, where standard AES67 gear marks PTP 46 — a documented "
            "QoS conflict to watch for on shared networks, though this "
            "driver marks its own transmit traffic with Dante's audio "
            "value, 46.";
        break;

    case CompatibilityProfileKind::CP850:
        p.displayName = "Dolby CP850 (Atmos Cinema Processor)";
        // Source: Docs/references/dolby_cp850_installation_manual_issue2.pdf
        // (Issue 2, 2014, part 9111710) — read in full, no AES67/PTP/
        // multicast/DSCP/BLU-Link content anywhere in its 105 pages. That's
        // not an oversight on this driver's part: this particular manual
        // covers the ORIGINAL CP850 + DAC3201 pairing, a fixed point-to-
        // point protocol ("Do not connect this port to an Ethernet
        // switch") entirely distinct from AES67 — confirmed by the DMA
        // manual's own warning that DAC3201 "uses a different protocol
        // that is not supported by the Dolby Multichannel Amplifier or
        // DAC3202." AES67/Atmos Connect exists on the CP850 as a separate
        // licensed "enablement" this manual doesn't itself configure.
        //
        // That the AES67 mode is real, not just a name on a spec sheet, is
        // now independently confirmed: Docs/references/
        // merging_cp850_aes67_config_notes.md (a third-party interop guide,
        // tested against CP850 firmware V2.3.1.4) walks through enabling it
        // via System > Network > Dolby Atmos Connect tab with its "legacy
        // mode" checkbox unticked, and confirms — a third source, after the
        // DMA and CP950/CP950A manuals — that the CP850 is meant to win PTP
        // grandmaster and the downstream device should be slave, matching
        // ptpRole below. Its own worked example uses custom PTP domain/
        // priority/multicast/port values, though, chosen for a specific
        // non-Dolby receiver via the "advanced mode" CP950/CP950A's manual
        // describes for exactly that purpose — not Dolby's own factory
        // defaults, so they aren't adopted here (see that file for why).
        //
        // Net effect: most parameters below remain inherited from the
        // CP950/CP950A and DMA manuals' shared-family documentation
        // (CP950/CP950A being CP850's own confirmed successor line) rather
        // than confirmed by a CP850-specific Dolby AES67 configuration
        // guide — genuinely not published anywhere by Dolby for CP850,
        // not merely not yet looked for. recommendedPtpDomain is the one
        // exception: every Dolby product in this family that documents a
        // PTP domain default (DMA, CP950/CP950A, DAC3202, all above) ships
        // at 109, so it's recorded for CP850 too rather than left at -1 —
        // still a factory default installers routinely override per
        // auditorium (109, 110, 111, 112, ... across multiple screens on
        // one network), never fixed.
        //
        // Digital cinema audio (DCI spec): 48 or 96 kHz, up to 24-bit PCM.
        // Not AES67's 44.1 kHz — cinema doesn't use it.
        p.allowedSampleRates = {48000.0, 96000.0};
        p.allowedPtimesUs = {1000}; // 1 ms
        p.allowedEncodings = {"L16", "L24"};
        p.maxChannelsPerFlow = 8;
        p.requiresZeroRtpTimestampOffset = false;
        p.domainIsFixed = false; // no fixed domain; cinema installs set their own house PTP domain per auditorium
        p.recommendedPtpDomain = 109; // same factory default as the rest of the Dolby Atmos Connect family (DMA/CP950A/DAC3202)
        p.recommendedDscp = 46;  // EF — Dolby's factory-default DSCP marking for AES67 traffic on this line
        // From our driver's point of view: the CP850 renders and sends its
        // feeds over AES67, it doesn't accept AES67 input. We can only
        // receive from it — up to 64 channels, the most it can render.
        p.direction = ProfileDirection::ReceiveOnly;
        p.maxTotalChannels = 64;
        p.ptpRole = PTPRoleConstraint::ForcedSlave;
        p.caveats =
            "The CP850 uses AES67 as its transport to Dolby Atmos Connect "
            "Interfaces (DAC3202), not the full Dante protocol — and not "
            "the DAC3201 breakout box (an older, incompatible point-to-"
            "point protocol; needs a separate licensed enablement to "
            "speak AES67 at all). Dolby's own documentation notes its "
            "factory-default DSCP marking is more traditional than "
            "typical Dante configurations (EF/46) — this driver has a "
            "DSCP-setting function (NetworkUtils::setQoSTrafficClass) but "
            "which this driver now applies to its own transmit sockets. PTP "
            "domain ships at 109, the same factory default as the rest of "
            "this Dolby family (not fixed — cinema installations "
            "routinely set their own per auditorium, e.g. "
            "109/110/111/112 across multiple screens). Otherwise, unlike "
            "CP950/CP950A/DMA/DAC3202, no CP850-specific Dolby manual "
            "documents its AES67 mode's own multicast address default "
            "(a third-party interop guide confirms the mode is "
            "real and where to enable it, System > Network > Dolby Atmos "
            "Connect tab, but its example uses custom non-Dolby values, "
            "not Dolby's own factory defaults), so these parameters "
            "remain inherited from that shared family rather than "
            "independently confirmed for CP850. This driver is always "
            "PTP slave under this profile — it never contends "
            "for grandmaster. Receive-only: this driver may only add RX "
            "streams under this profile, up to 64 channels total, the "
            "most the CP850 renders.";
        break;

    case CompatibilityProfileKind::CP950:
        p.displayName = "Dolby CP950 / CP950A (Cinema Processor)";
        // Source: Docs/references/dolby_cp950_cp950a_manual.pdf (Dolby
        // Cinema Processor CP950 and Dolby Atmos Cinema Processor CP950A
        // Manual, Issue 13). The manual states outright that a CP950/
        // CP950A replaces CP850/CP750/CP650 installs — same role from this
        // driver's own point of view as CP850: it renders and sends, this
        // driver only ever receives.
        p.allowedSampleRates = {48000.0, 96000.0}; // same DCI parameters as CP850
        p.allowedPtimesUs = {1000}; // 1 ms
        p.allowedEncodings = {"L16", "L24"};
        p.maxChannelsPerFlow = 8;
        p.requiresZeroRtpTimestampOffset = false;
        p.domainIsFixed = false; // no fixed domain — matches CP850's own installs-set-their-own-domain note
        p.recommendedPtpDomain = 109; // confirmed default, §3.8 "PTP domain number"
        p.recommendedMulticastAddress = "239.81.83.67"; // §3.8 "Destination multicast IP", shared with DMA/DAC3202
        // From our driver's point of view: same as CP850, we only ever
        // receive. CP950 outputs up to 16ch over Atmos Connect, CP950A up
        // to 64ch — one profile covers both, the real unit's channel count
        // is whatever the user picks with the Input selector, same
        // treatment as the DMA profile's Output selector.
        p.direction = ProfileDirection::ReceiveOnly;
        p.maxTotalChannels = 64;
        // §3.8 "PTP priority": the CP950/CP950A defaults to PTP priority 1
        // = 127 (lower number wins BMCA), while other Dolby devices
        // default to priority 128 — i.e. it wins grandmaster by default.
        // This driver, receiving from it, takes the complementary role.
        p.ptpRole = PTPRoleConstraint::ForcedSlave;
        p.caveats =
            "Current-generation replacement for CP850 (its own manual says "
            "so). CP950 renders up to 16 channels over Atmos Connect, "
            "CP950A up to 64 — pick your real unit's channel count with "
            "the Input selector rather than a separate profile per model. "
            "Atmos Connect can carry AES67 or BLU Link, mutually exclusive "
            "per installation (the unit reboots when switching between "
            "them) — this profile assumes AES67 mode, the factory default. "
            "PTP domain ships at 109 (not fixed — installers commonly set "
            "a different one per auditorium, e.g. 109/110/111, to keep "
            "multiple screens on the same network from colliding); "
            "factory-default destination multicast address 239.81.83.67, "
            "shared out of the box with Dolby Multichannel Amplifier and "
            "DAC3202 — likewise expected to be changed per auditorium in "
            "multi-screen installs. This driver is always PTP slave under "
            "this profile — the CP950/CP950A ships at the highest PTP "
            "priority in the chain and is meant to win grandmaster. "
            "Receive-only: this driver may only add RX streams under this "
            "profile, up to 64 channels total.";
        break;

    case CompatibilityProfileKind::DAC3202:
        p.displayName = "Dolby DAC3202 (Atmos Connect Interface)";
        // Receiving end of the same CP850/CP950/CP950A link — same audio
        // parameters.
        p.allowedSampleRates = {48000.0, 96000.0};
        p.allowedPtimesUs = {1000}; // 1 ms
        p.allowedEncodings = {"L16", "L24"};
        // 32 analog outputs per interface — exactly 4 flows at this
        // driver's 8-channel-per-flow limit, not a coincidence: AES67
        // itself is why the DAC3202 is organized that way.
        p.maxChannelsPerFlow = 8;
        p.requiresZeroRtpTimestampOffset = false;
        p.domainIsFixed = false;
        p.recommendedDscp = 46;
        // Confirmed (not assumed) by the CP950/CP950A manual, which names
        // the DAC3202 explicitly alongside DMA for both: same 109 PTP
        // domain default, same 239.81.83.67 destination multicast default,
        // same fixed-port/stepped-source-port Atmos Connect wire scheme
        // DMA already used — no longer DMA-only speculation extended here,
        // it's the confirmed shared scheme.
        p.recommendedPtpDomain = 109;
        p.recommendedMulticastAddress = "239.81.83.67";
        p.useFixedMulticastWithPerFlowSourcePort = true;
        p.maxUnits = 3; // DMA manual §2.3: at most three chained without a switch
        // From our driver's point of view: the DAC3202 only converts
        // digital audio to analog outputs — it has no network input for
        // audio to come back to us. We can only transmit to it.
        p.direction = ProfileDirection::TransmitOnly;
        p.maxTotalChannels = 32;
        p.ptpRole = PTPRoleConstraint::ForcedMaster;
        p.caveats =
            "Same link as CP850/CP950/CP950A (above), receiving end — 32 "
            "analog outputs, so a full-width feed to one DAC3202 is 4 "
            "flows of 8 channels. Multi-flow addressing matches the real "
            "device (confirmed by the CP950/CP950A manual): one multicast "
            "address, fixed RTP destination port (pass 6517 to match "
            "Dolby's own default), source port stepped per 8-channel flow "
            "— see StreamManager::createTxStreamFlows(). Same DSCP note as "
            "CP850: factory default EF/46, which this driver applies to its "
            "own transmit sockets. PTP domain ships at 109; factory-default "
            "destination multicast address 239.81.83.67 — both commonly "
            "changed per auditorium in multi-screen installs, same as "
            "CP950/CP950A above. This driver is always PTP master under "
            "this profile. Transmit-only: this driver may only create TX "
            "streams under this profile, up to 32 channels "
            "total, its full analog output count.";
        break;

    case CompatibilityProfileKind::DMA:
        p.displayName = "Dolby DMA (Multichannel Amplifier)";
        // Source: Docs/references/dolby_multichannel_amplifier_manual.pdf
        // (Dolby Multichannel Amplifier User Manual, Issue 7). Real units
        // come in fixed-channel models (DMA16301/16302, DMA24300/24302,
        // DMA32300/32301, and multiple units can be combined for more), but
        // this profile doesn't hardcode a model: unlike CP850/DAC3202's
        // maxTotalChannels, this driver's own output count is user-
        // selectable (ContentView's Output channel-count selector) rather
        // than fixed, so one profile covers every model — just pick the
        // real amplifier's channel count there. maxTotalChannels here is
        // the outer ceiling that selector is clamped to, not a specific
        // model's count.
        //
        // PTP: confirmed, not assumed. §3.2.4 "PTP Domain Number": "The
        // Dolby Multichannel Amplifier does not allow you to set the PTP
        // clock priority. It is always 255 (or the lowest priority in the
        // chain). It should never be chosen as the source of the clock for
        // the network. It is strictly a downstream device." This driver, in
        // this profile, plays the cinema-processor role sending TO a DMA —
        // so it takes the complementary role: always PTP master.
        //
        // PTP domain: same section documents the factory default as 109
        // (not 0) for a single-auditorium install; it must match the
        // sending processor's domain and installers are expected to change
        // it per auditorium, so it's not literally fixed — recorded as
        // recommendedPtpDomain, not domainIsFixed.
        //
        // Sample rate/ptime/encoding: not stated in the manual — the DMA
        // just decodes whatever its Dolby Atmos Connect input carries, which
        // is set by the upstream processor (CP850/CP950A/IMS3000). Copied
        // from CP850/DAC3202's DCI parameters (48/96 kHz, 1 ms, L16/L24) as
        // the same-chain assumption, not independently confirmed here.
        //
        // Channel ceiling: 32. A single DMA is a 16-, 24-, or 32-channel
        // model (DMA16/24/32), and this driver feeds ONE unit at a time —
        // the amplifier-unit selector picks WHICH unit by stepping the
        // source-port offset, it does not sum units into one wider stream.
        // So the general per-unit ceiling is the largest single model, 32,
        // the same as the DAC3202. (An earlier 64 came from one specific
        // install that combined two 32-channel units; that is a site
        // configuration, not the general profile — chaining is expressed by
        // maxUnits + the port offset, not by widening this ceiling.)
        //
        // Port scheme: §3.2.4 "Source UDP and RTP Destination Ports"
        // documents a FIXED RTP destination port (6517) with the SOURCE UDP
        // port stepped per 8-channel block (6518, 6519, 6520, 6521, ...) —
        // one multicast address, ports distinguish the flows. Every other
        // profile's flow splitter instead steps the destination multicast
        // IP's last octet per flow and keeps ports fixed (the AES67/Dante
        // convention) — see useFixedMulticastWithPerFlowSourcePort below,
        // which switches StreamManager::createTxStreamFlows() to Dolby's
        // scheme for this profile.
        p.allowedSampleRates = {48000.0, 96000.0};
        p.allowedPtimesUs = {1000}; // 1 ms
        p.allowedEncodings = {"L16", "L24"};
        p.maxChannelsPerFlow = 8;
        p.requiresZeroRtpTimestampOffset = false;
        p.domainIsFixed = false;
        p.recommendedPtpDomain = 109;
        // Not stated in the DMA's own manual — confirmed instead by the
        // CP950/CP950A manual, which names "Dolby Multichannel Amplifier"
        // explicitly alongside CP950/CP950A and DAC3202 as sharing this
        // default (see DAC3202's own case above for the same citation).
        p.recommendedMulticastAddress = "239.81.83.67";
        p.recommendedDscp = -1; // manual recommends DiffServ QoS (4 queues, strict priority) on the switch, not one documented codepoint
        p.direction = ProfileDirection::TransmitOnly;
        p.maxTotalChannels = 32; // largest SINGLE DMA model (DMA32); 16/24 are the smaller models, all user-selectable below
        p.ptpRole = PTPRoleConstraint::ForcedMaster;
        // Real Atmos Connect wire scheme, not this driver's default AES67/
        // Dante one — see useFixedMulticastWithPerFlowSourcePort's own doc
        // comment. StreamManager::createTxStreamFlows() reads this to keep
        // one multicast address and the fixed destination port across every
        // flow, stepping the source port instead.
        p.useFixedMulticastWithPerFlowSourcePort = true;
        p.maxUnits = 3; // §2.3: at most three chained without a switch
        p.caveats =
            "Covers the whole Dolby Multichannel Amplifier family "
            "(DMA16301/16302, DMA24300/24302, DMA32300/32301) — pick your "
            "real amplifier's channel count with the Output selector on the "
            "main window rather than a separate profile per model; 32 is the "
            "ceiling, the largest single model (DMA32), since this driver "
            "feeds one unit at a time. Combining units for more channels is a "
            "specific install, handled by the amplifier-unit selector (up to "
            "three), not by a wider single stream. Multi-flow addressing "
            "matches the real "
            "device: one multicast address, fixed RTP destination port "
            "(pass 6517 as the stream's port to match Dolby's own default), "
            "source port stepped per 8-channel flow (destination+1, +2, "
            "+3, ...) — see StreamManager::createTxStreamFlows(). Sample "
            "rate/ptime/encoding are inherited from the same Atmos Connect "
            "chain as CP850/DAC3202 (not independently documented for the "
            "DMA itself). PTP domain ships at 109 for Dolby gear (not "
            "fixed — installers commonly set a different one per "
            "auditorium in multi-screen installs); factory-default "
            "destination multicast address 239.81.83.67. This driver is "
            "always PTP master under this profile, transmit-only.";
        break;
    }

    return p;
}

std::vector<CompatibilityProfile> CompatibilityProfile::all() {
    return {
        forKind(CompatibilityProfileKind::AES67),
        forKind(CompatibilityProfileKind::RAVENNA),
        forKind(CompatibilityProfileKind::ST2110_30),
        forKind(CompatibilityProfileKind::ST2110_30_LevelB),
        forKind(CompatibilityProfileKind::Dante),
        forKind(CompatibilityProfileKind::CP850),
        forKind(CompatibilityProfileKind::CP950),
        forKind(CompatibilityProfileKind::DAC3202),
        forKind(CompatibilityProfileKind::DMA),
    };
}

std::string CompatibilityProfile::kindToString(CompatibilityProfileKind kind) {
    switch (kind) {
    case CompatibilityProfileKind::AES67:     return "aes67";
    case CompatibilityProfileKind::RAVENNA:   return "ravenna";
    case CompatibilityProfileKind::ST2110_30: return "st2110-30";
    case CompatibilityProfileKind::ST2110_30_LevelB: return "st2110-30-b";
    case CompatibilityProfileKind::Dante:     return "dante";
    case CompatibilityProfileKind::CP850:     return "cp850";
    case CompatibilityProfileKind::CP950:     return "cp950";
    case CompatibilityProfileKind::DAC3202:   return "dac3202";
    case CompatibilityProfileKind::DMA:       return "dma";
    }
    return "aes67";
}

CompatibilityProfileKind CompatibilityProfile::kindFromString(const std::string& s) {
    if (s == "ravenna")   return CompatibilityProfileKind::RAVENNA;
    if (s == "st2110-30") return CompatibilityProfileKind::ST2110_30;
    if (s == "st2110-30-b") return CompatibilityProfileKind::ST2110_30_LevelB;
    if (s == "dante")     return CompatibilityProfileKind::Dante;
    if (s == "cp850")     return CompatibilityProfileKind::CP850;
    if (s == "cp950")     return CompatibilityProfileKind::CP950;
    if (s == "dac3202")   return CompatibilityProfileKind::DAC3202;
    if (s == "dma")       return CompatibilityProfileKind::DMA;
    return CompatibilityProfileKind::AES67;
}

// ============================================================================
// Validation
// ============================================================================

namespace {

std::string joinRates(const std::vector<double>& rates) {
    std::ostringstream oss;
    for (size_t i = 0; i < rates.size(); ++i) {
        if (i) oss << ", ";
        oss << static_cast<long>(rates[i]);
    }
    return oss.str();
}

std::string joinStrings(const std::vector<std::string>& items) {
    std::ostringstream oss;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) oss << ", ";
        oss << items[i];
    }
    return oss.str();
}

bool addressHasPrefix(const std::string& address, const std::string& prefix) {
    // Prefix is a dotted fragment like "239.69"; require a following dot so
    // "239.6" doesn't match "239.69.x.x".
    if (address.size() <= prefix.size()) return false;
    return address.compare(0, prefix.size(), prefix) == 0 &&
           address[prefix.size()] == '.';
}

} // namespace

bool CompatibilityProfile::validate(const SDPSession& sdp, bool isTransmit, std::string* errorOut) const {
    auto fail = [&](const std::string& reason) {
        if (errorOut) *errorOut = displayName + ": " + reason;
        return false;
    };

    if (direction == ProfileDirection::ReceiveOnly && isTransmit) {
        return fail("this device has no network audio input — this driver may only receive from it, not send to it");
    }
    if (direction == ProfileDirection::TransmitOnly && !isTransmit) {
        return fail("this device has no network audio output — this driver may only send to it, not receive from it");
    }

    if (!allowedSampleRates.empty()) {
        const bool ok = std::any_of(allowedSampleRates.begin(), allowedSampleRates.end(),
            [&](double rate) {
                // SDP rates are integers in practice; compare with a
                // tolerance rather than exact double equality.
                return std::abs(rate - sdp.sampleRate) < 1.0;
            });
        if (!ok) {
            return fail("sample rate " + std::to_string(static_cast<long>(sdp.sampleRate)) +
                        " Hz not permitted (allowed: " + joinRates(allowedSampleRates) + ")");
        }
    }

    if (!allowedPtimesUs.empty() && sdp.ptimeUs > 0) {
        const bool ok = std::find(allowedPtimesUs.begin(), allowedPtimesUs.end(),
                                   sdp.ptimeUs) != allowedPtimesUs.end();
        if (!ok) {
            return fail("packet time " + std::to_string(sdp.ptimeUs) +
                        " us not permitted");
        }
    }

    if (!allowedEncodings.empty() && !sdp.encoding.empty()) {
        const bool ok = std::find(allowedEncodings.begin(), allowedEncodings.end(),
                                   sdp.encoding) != allowedEncodings.end();
        if (!ok) {
            return fail("encoding " + sdp.encoding + " not permitted (allowed: " +
                        joinStrings(allowedEncodings) + ")");
        }
    }

    if (sdp.numChannels > maxChannelsPerFlow) {
        return fail(std::to_string(sdp.numChannels) + " channels exceeds the " +
                    std::to_string(maxChannelsPerFlow) +
                    "-channel flow limit — split it across multiple flows");
    }

    if (!requiredMulticastPrefix.empty() && !sdp.connectionAddress.empty()) {
        if (!addressHasPrefix(sdp.connectionAddress, requiredMulticastPrefix)) {
            return fail("multicast address " + sdp.connectionAddress +
                        " outside the required " + requiredMulticastPrefix + ".0.0/16 range");
        }
    }

    // -1 means "no PTP for this stream" — not a domain choice at all, so it
    // isn't subject to a fixed-domain requirement.
    if (domainIsFixed && sdp.ptpDomain != -1 && sdp.ptpDomain != static_cast<int>(fixedDomain)) {
        return fail("PTP domain " + std::to_string(sdp.ptpDomain) + " not permitted — "
                    "fixed at " + std::to_string(fixedDomain));
    }

    return true;
}

// ============================================================================
// Persistence
// ============================================================================

CompatibilityProfileManager::CompatibilityProfileManager() {
    std::string existing = findExistingConfig();
    configPath_ = existing.empty()
        ? "/Library/Application Support/AES67Driver/" + std::string(kDefaultConfigFile)
        : existing;
}

CompatibilityProfileManager::~CompatibilityProfileManager() = default;

std::string CompatibilityProfileManager::getConfigPath() const { return configPath_; }

std::vector<std::string> CompatibilityProfileManager::getConfigSearchPaths() {
    std::vector<std::string> paths;

    const char* envPath = std::getenv("AES67_COMPAT_PROFILE_CONFIG_PATH");
    if (envPath && envPath[0] != '\0') paths.push_back(envPath);

    const char* home = std::getenv("HOME");
    if (!home) {
        struct passwd* pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }
    if (home && home[0] != '\0') {
        paths.push_back(std::string(home) + "/Library/Application Support/AES67Driver/" + kDefaultConfigFile);
    }

    paths.push_back("/Library/Application Support/AES67Driver/" + std::string(kDefaultConfigFile));
    return paths;
}

std::string CompatibilityProfileManager::findExistingConfig() {
    for (const auto& path : getConfigSearchPaths()) {
        struct stat st;
        if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) return path;
    }
    return "";
}

bool CompatibilityProfileManager::ensureConfigDirectoryExists() {
    size_t lastSlash = configPath_.find_last_of('/');
    if (lastSlash == std::string::npos) return false;
    std::string dir = configPath_.substr(0, lastSlash);

    struct stat st;
    if (stat(dir.c_str(), &st) == 0) return S_ISDIR(st.st_mode);

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        AES67_LOGF("CompatibilityProfileManager: Failed to create directory '%s': %s",
                   dir.c_str(), ec.message().c_str());
        return false;
    }
    chmod(dir.c_str(), 0755);
    return true;
}

CompatibilityProfileKind CompatibilityProfileManager::load() {
    std::ifstream file(configPath_);
    if (!file.is_open()) return CompatibilityProfileKind::AES67;

    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string json = buffer.str();

    std::regex pattern("\"profile\"\\s*:\\s*\"([^\"]*)\"");
    std::smatch match;
    if (std::regex_search(json, match, pattern) && match.size() > 1) {
        const auto kind = CompatibilityProfile::kindFromString(match[1].str());
        AES67_LOGF("CompatibilityProfileManager: Loaded profile '%s' from %s",
                   CompatibilityProfile::kindToString(kind).c_str(), configPath_.c_str());
        return kind;
    }

    return CompatibilityProfileKind::AES67;
}

bool CompatibilityProfileManager::save(CompatibilityProfileKind kind) {
    if (!ensureConfigDirectoryExists()) {
        AES67_LOG("CompatibilityProfileManager: Failed to create config directory");
        return false;
    }

    std::ostringstream json;
    json << "{\n";
    json << "  \"version\": \"1.0\",\n";
    json << "  \"profile\": \"" << CompatibilityProfile::kindToString(kind) << "\"\n";
    json << "}\n";

    std::ofstream file(configPath_);
    if (!file.is_open()) {
        AES67_LOGF("CompatibilityProfileManager: Failed to open %s for writing", configPath_.c_str());
        return false;
    }
    file << json.str();
    return true;
}

} // namespace AES67
