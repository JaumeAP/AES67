//
// SourceGen.h
// aes67-linux-driver
// The daemon's RTP sources, written from a compatibility profile.
//
// A source is what the daemon sends: an ALSA playback device read into RTP
// packets. Its body is documented in daemon/README.md, and four of its fields
// are things a profile knows -- the codec, the samples in a packet, the DSCP
// marking of the media and the destination address. The rest are upstream's
// own example values, which is what the daemon ships with.
//
// The profile's recommendedDscp belongs here and not in daemon.conf: this is
// the marking of the audio, and daemon.conf's ptp_dscp marks PTP.
//
#pragma once

#include "Profiles/CompatibilityProfile.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace AES67::LinuxDriver {

struct SourceRequest {
    CompatibilityProfileKind kind{CompatibilityProfileKind::AES67};
    int count{1};                        ///< How many sources to write.
    int channelsPerSource{2};            ///< Channels each one carries.
    int startId{0};                      ///< The id the first one takes.
    std::optional<double> sampleRate;
    std::optional<uint32_t> ptimeUs;
    std::optional<std::string> codec;
};

struct SourceResult {
    bool ok{false};
    std::string json;                    ///< { "sources": [ ... ] }, when ok.
    std::string error;
    std::vector<std::string> warnings;   ///< What was adjusted, and to what.
};

/// Writes the sources `request` asks for, or says why it cannot.
SourceResult generateSources(const SourceRequest& request);

} // namespace AES67::LinuxDriver
