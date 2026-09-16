//
// SdpFixture.h
// AES67 core - test support
// The session and the mapping a test starts from when what it is testing is
// not the parsing.
//
// Written identically in aes67-core's TestStreamConfig and the driver's
// TestStreamManager: the same eleven fields, the same 239.1.1.1, the same
// L24 at 48 kHz. Two copies of a fixture drift, and when they do the two
// suites stop testing the same stream while still looking as though they do.
//
#pragma once

#include "Driver/SDPParser.h"
#include "NetworkEngine/StreamChannelMapper.h"

#include <cstdint>
#include <string>

namespace AES67 {
namespace TestSupport {

/// A well-formed two-channel L24 session at 48 kHz, 1 ms packets.
inline SDPSession createTestSDP(const std::string& name = "Test Stream",
                                uint16_t port = 5004,
                                uint16_t channels = 2,
                                uint32_t sampleRate = 48000) {
    SDPSession sdp;
    sdp.sessionName = name;
    sdp.port = port;
    sdp.connectionAddress = "239.1.1.1";
    sdp.encoding = "L24";
    sdp.sampleRate = sampleRate;
    sdp.numChannels = channels;
    sdp.payloadType = 97;
    sdp.ptimeUs = 1000;
    sdp.framecount = 48;
    sdp.originAddress = "192.168.1.100";
    sdp.ptpDomain = 0;
    return sdp;
}

/// The mapping that takes all of that session's channels to consecutive
/// device channels from `deviceStart`.
inline ChannelMapping createTestMapping(uint16_t streamChannels = 2,
                                        uint16_t deviceStart = 0) {
    ChannelMapping mapping;
    mapping.streamID = StreamID::generate();
    mapping.streamName = "Test Mapping";
    mapping.streamChannelCount = streamChannels;
    mapping.streamChannelOffset = 0;
    mapping.deviceChannelStart = deviceStart;
    mapping.deviceChannelCount = streamChannels;
    return mapping;
}

} // namespace TestSupport
} // namespace AES67
