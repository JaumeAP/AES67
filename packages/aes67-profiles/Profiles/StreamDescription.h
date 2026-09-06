//
// StreamDescription.h
// AES67 profiles
// The little a profile needs to know about a stream to say whether it accepts
// it.
//
// A compatibility profile validates six things: the address, the sample rate,
// the packet time, the encoding, the channel count and the PTP domain. It used
// to take an SDPSession to read them, which meant this package would have to
// know what SDP is, and through it the parser, and through that the core --
// the whole dependency the other way round from where it belongs. A profile is
// a table of constraints; it has no business knowing where the numbers it
// checks came from.
//
// So it takes these six fields. Whoever holds a session description fills them
// in: the macOS driver's core does it from an SDPSession in one function, and
// a firmware with no SDP at all can fill them from whatever it does have.
//
#pragma once

#include <cstdint>
#include <string>

namespace AES67 {

struct StreamDescription {
    /// The destination the stream is sent to. Some profiles require a
    /// particular multicast range.
    std::string connectionAddress;
    /// "L16", "L24", "AM824" -- what the payload carries.
    std::string encoding;
    double sampleRate{0.0};
    uint16_t numChannels{0};
    /// Packet time in microseconds. Zero means unknown, and a profile that
    /// constrains it lets an unknown one through rather than guessing.
    uint32_t ptimeUs{0};
    /// The PTP domain the stream says it is timed by.
    int ptpDomain{0};
};

} // namespace AES67
