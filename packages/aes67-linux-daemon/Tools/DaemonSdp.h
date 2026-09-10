//
// DaemonSdp.h
// aes67-linux-daemon
// The SDP the vendored daemon announces, written the way it writes it.
//
// This mirrors SessionManager::get_source_sdp_ in the AES67 Linux daemon,
// bondagit/aes67-linux-daemon, daemon/session_manager.cpp, which cannot be
// called from here: it is a private method of a class that owns a netlink
// handle, a driver and a PTP state, and its source is not vendored in this
// tree. What it emits, though, is a function of a
// dozen values, and those are what this takes.
//
// It is a mirror, so it drifts if upstream changes. TestDaemonSdp pins it to
// the SDP the daemon's own README documents, which is what catches the drift
// when the submodule moves.
//
#pragma once

#include <cstdint>
#include <string>

namespace AES67::LinuxDriver {

/// Everything get_source_sdp_ reads, flattened: the source's own fields, the
/// daemon's configuration and its PTP state.
struct DaemonSdpParams {
    std::string nodeId{"AES67 daemon d9aca383"};  ///< config, "s=" prefix
    std::string sourceName{"ALSA Source 0"};
    std::string sourceIp{"127.0.0.1"};       ///< config ip_addr, the "o=" address
    std::string destinationIp{"239.1.0.1"};  ///< the stream's destination
    uint16_t destinationPort{5004};
    uint8_t ttl{15};
    uint8_t payloadType{98};
    std::string codec{"L16"};
    uint8_t channels{2};
    uint32_t sampleRate{44100};
    uint32_t maxSamplesPerPacket{48};
    uint8_t ptpDomain{0};
    bool refclkPtpTraceable{true};
    std::string gmid{"00-00-00-00-00-00-00-00"};  ///< used when not traceable
    uint64_t sessionId{0};
    uint64_t sessionVersion{0};
};

/// The SDP the daemon would announce for that source.
std::string daemonSdp(const DaemonSdpParams& params);

/// The daemon's own a=ptime: samples per packet over the rate, in
/// milliseconds, printed to twelve decimals with trailing zeros and any
/// trailing dot removed (session_manager.cpp:744-752).
std::string daemonPtime(uint32_t maxSamplesPerPacket, uint32_t sampleRate);

} // namespace AES67::LinuxDriver
