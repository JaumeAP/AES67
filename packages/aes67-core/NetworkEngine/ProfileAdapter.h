//
// ProfileAdapter.h
// AES67
// How this core hands a session description to a profile.
//
// The profiles are their own package and know nothing about SDP: a
// compatibility profile validates six values -- address, encoding, sample
// rate, channel count, packet time and PTP domain -- and where they came from
// is not its business. This is the one function that fills those six in from
// an SDPSession, and it lives here because SDP is this core's concern.
//
#pragma once

#include "Driver/SDPParser.h"
#include "Profiles/StreamDescription.h"

namespace AES67 {

inline StreamDescription describeStream(const SDPSession& sdp) {
    StreamDescription stream;
    stream.connectionAddress = sdp.connectionAddress;
    stream.encoding = sdp.encoding;
    stream.sampleRate = sdp.sampleRate;
    stream.numChannels = sdp.numChannels;
    stream.ptimeUs = sdp.ptimeUs;
    stream.ptpDomain = sdp.ptpDomain;
    return stream;
}

} // namespace AES67
