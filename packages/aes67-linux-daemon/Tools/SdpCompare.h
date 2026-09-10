//
// SdpCompare.h
// aes67-linux-daemon
// The daemon's SDP against this project's, field by field.
//
// Both sides describe the same stream: same address, port, codec, rate,
// channels, packet time, PTP domain. What differs is how each writes it down,
// and the question this answers is which of those differences a receiver would
// notice -- one that changes what is on the wire or how it is found, against
// one that is a spelling.
//
#pragma once

#include "Driver/SDPParser.h"
#include "Tools/DaemonSdp.h"

#include <string>
#include <vector>

namespace AES67::LinuxDriver {

struct SdpDifference {
    std::string field;    ///< The SDP line or attribute.
    std::string daemon;   ///< What the daemon writes, or empty if it writes none.
    std::string ours;     ///< What SDPParser::generate writes, or empty.
    bool breaking{false}; ///< True when a receiver would behave differently.
    std::string note;     ///< Why it matters, or why it does not.
};

/// The same stream, described the daemon's way.
std::string daemonSdpFor(const DaemonSdpParams& params);

/// The same stream, described this project's way.
std::string ourSdpFor(const DaemonSdpParams& params);

/// Every field the two disagree on, in the order the SDP writes them.
std::vector<SdpDifference> compareSdp(const DaemonSdpParams& params);

/// True when any difference would change what a receiver does.
bool hasBreaking(const std::vector<SdpDifference>& differences);

} // namespace AES67::LinuxDriver
