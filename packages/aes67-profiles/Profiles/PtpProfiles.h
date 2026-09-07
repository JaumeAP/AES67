//
// PtpProfiles.h
// AES67 profiles
// The PTP profiles, as data: what each ecosystem expects a port to announce
// and to send at.
//
// A PTP profile is not an algorithm. It is five numbers -- a domain, a
// majorSdoId and three log2-second intervals -- that an ecosystem agrees on,
// and there is nothing to run: the port that implements the protocol takes
// them and behaves accordingly.
//
// Both implementations in this repository read this file. The Teensy library
// carried its own copy inside an applyProfile() switch and carries none now;
// the macOS side never had one. A table of somebody else's numbers has no
// business living inside a port.
//
// Freestanding on purpose: no std::string, no std::vector, no allocation, no
// operating system. This is included by a Cortex-M7 firmware as readily as by
// a macOS driver, and anything heavier here would make that a lie.
//
// What a profile does NOT set: priority1, priority2 and the clock quality
// (they describe the clock, not the ecosystem), the delay mechanism (end to
// end or peer to peer, which is how the port was built), and the transport
// (UDP for AES67, raw Ethernet for 802.1AS).
//
#pragma once

#include <cstddef>
#include <cstdint>

namespace AES67 {

/// The five numbers a profile fixes.
///
/// The intervals are log2 seconds, as IEEE 1588 carries them on the wire: 0 is
/// one per second, -3 is eight per second, 1 is one every two seconds.
struct PtpProfileSettings {
    uint8_t domainNumber{0};
    /// The top nibble of octet 0. Zero is the default profile AES67 builds on;
    /// 1 is 802.1AS, and a receiver following that profile drops everything
    /// that does not carry it.
    uint8_t majorSdoId{0};
    int8_t logSyncInterval{0};
    int8_t logAnnounceInterval{1};
    int8_t logMinDelayReqInterval{0};
};

struct PtpProfile {
    const char* name;
    const char* description;
    PtpProfileSettings settings;
};

/// IEEE 1588-2008's delay request-response default profile: Sync every second,
/// Announce every two, Delay_Req every second.
inline constexpr PtpProfile kPtpDefaultProfile{
    "default1588",
    "IEEE 1588-2008 default delay request-response profile",
    {0, 0, 0, 1, 0},
};

/// What AES67 and RAVENNA gear runs: Sync and Delay_Req eight times a second,
/// Announce once, domain 0.
inline constexpr PtpProfile kPtpAes67MediaProfile{
    "aes67",
    "The media profile AES67 and RAVENNA gear runs",
    {0, 0, -3, 0, -3},
};

/// The media profile at twice the Sync rate: sixteen a second, Announce once,
/// Delay_Req sixteen. Same ecosystem as `aes67` -- domain 0, majorSdoId 0 --
/// for a network kept well enough that the extra traffic buys precision
/// instead of jitter. A master announcing this has to send at the rate it
/// announces, which is the reason the number lives here and not in a
/// consumer's own table.
inline constexpr PtpProfile kPtpAes67TightProfile{
    "aes67-tight",
    "The media profile at sixteen Sync a second, for well kept networks",
    {0, 0, -4, 0, -4},
};

/// 802.1AS: Sync eight times a second, Announce once, Pdelay_Req once a
/// second, and majorSdoId 1.
inline constexpr PtpProfile kPtpGptpProfile{
    "gptp",
    "802.1AS, which shares the wire and takes a majorSdoId of its own",
    {0, 1, -3, 0, 0},
};

/// Every profile, for a menu or a command line. A plain array: this header is
/// included by firmware.
inline constexpr const PtpProfile* kPtpProfiles[] = {
    &kPtpDefaultProfile,
    &kPtpAes67MediaProfile,
    &kPtpAes67TightProfile,
    &kPtpGptpProfile,
};
inline constexpr size_t kPtpProfileCount =
    sizeof(kPtpProfiles) / sizeof(kPtpProfiles[0]);

/// Compares two names without <cstring>, so this stays usable in a constant
/// expression and on a target where pulling in the C library for one strcmp is
/// not worth it.
constexpr bool ptpProfileNameEquals(const char* a, const char* b) {
    while (*a != '\0' && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

/// By name, or nullptr. The names are what a person types: "default1588",
/// "aes67", "gptp".
constexpr const PtpProfile* ptpProfileByName(const char* name) {
    if (name == nullptr) return nullptr;
    for (size_t i = 0; i < kPtpProfileCount; ++i) {
        if (ptpProfileNameEquals(kPtpProfiles[i]->name, name)) return kPtpProfiles[i];
    }
    return nullptr;
}

} // namespace AES67
