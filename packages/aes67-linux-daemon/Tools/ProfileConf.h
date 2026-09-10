//
// ProfileConf.h
// aes67-linux-daemon
// Turning a compatibility profile into the daemon's configuration file.
//
// The daemon has its own daemon.conf and its own defaults, and this does not
// replace them: it takes the file upstream ships as the base and rewrites the
// handful of keys a profile actually determines -- the sample rate, the frame
// size that carries the packet time, the PTP domain, the multicast base and
// the interface. Everything else stays exactly as upstream wrote it, byte for
// byte, so upstream's defaults keep being upstream's.
//
#pragma once

#include "Profiles/CompatibilityProfile.h"

#include <cstdint>
#include <optional>
#include <string>

namespace AES67::LinuxDriver {

/// What the caller asks for. Everything but the profile is optional: left
/// empty, the profile's own values decide.
struct ConfRequest {
    CompatibilityProfileKind kind{CompatibilityProfileKind::AES67};
    std::optional<double> sampleRate;
    std::optional<uint32_t> ptimeUs;
    std::optional<std::string> interfaceName;
};

struct ConfResult {
    bool ok{false};
    std::string text;   ///< The rewritten daemon.conf, when ok.
    std::string error;  ///< Why not, otherwise.
};

/// Rewrites `base` -- upstream's daemon.conf -- for `request`.
///
/// Fails rather than guessing: a rate or a packet time the profile does not
/// allow, a packet time that is not a whole number of frames at 48 kHz, a key
/// the base file does not carry, or a profile that requires a multicast
/// prefix without documenting an address to use.
ConfResult applyProfile(const std::string& base, const ConfRequest& request);

/// True when `name` is one of the profile ids CompatibilityProfile knows.
/// kindFromString answers AES67 for anything it does not recognise, which
/// would turn a typo into a silently different configuration.
bool isKnownProfileName(const std::string& name);

} // namespace AES67::LinuxDriver
