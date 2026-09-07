//
// TestPacketBudget.cpp
// AES67 Core
//
// The budget is a handful of divisions, so what is worth checking is that
// the numbers are the ones the standards and the gear actually rely on: that
// AES67's eight channels at 1 ms fit, that RAVENNA's and Level C's 64 fit
// only at 125 us, and that the split the transmitter makes at 96 kHz is
// five and not eight.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/RTP/PacketBudget.h"

using namespace AES67::PacketBudget;

TEST_CASE("The frame is a plain Ethernet one") {
    // 1500 minus IPv4 and UDP, minus the 12-byte RTP header.
    CHECK(kMaxRtpPacketBytes == 1472);
    CHECK(kMaxAudioBytesPerPacket == 1460);
}

TEST_CASE("Sample widths are the wire's, and unknown encodings are zero") {
    CHECK(bytesPerSample("L16") == 2);
    CHECK(bytesPerSample("L24") == 3);
    CHECK(bytesPerSample("AM824") == 4);
    CHECK(bytesPerSample("L32") == 0);
    CHECK(bytesPerSample("") == 0);
}

TEST_CASE("Frames per packet follow framecount first, then ptime") {
    CHECK(framesPerPacket(48000, 1000, 0) == 48);
    CHECK(framesPerPacket(96000, 1000, 0) == 96);
    CHECK(framesPerPacket(48000, 125, 0) == 6);
    CHECK(framesPerPacket(44100, 1000, 0) == 44);   // truncated, as the RTP paths do
    CHECK(framesPerPacket(48000, 1000, 64) == 64);  // an explicit framecount wins
    CHECK(framesPerPacket(0, 1000, 0) == 0);
}

TEST_CASE("AES67's eight channels at a millisecond fit, and ten is the ceiling") {
    // 8 x 3 x 48 = 1152 audio bytes, 1164 with the header.
    CHECK(rtpPacketBytes(8, 3, 48) == 1164);
    CHECK(fits(8, 3, 48));
    CHECK(maxChannelsPerPacket(3, 48) == 10);
    CHECK(fits(10, 3, 48));
    CHECK(!fits(11, 3, 48));
    // L16 is narrower, so more of it fits.
    CHECK(maxChannelsPerPacket(2, 48) == 15);
}

TEST_CASE("Sixty-four channels fit at 125 us and at no millisecond") {
    // What RAVENNA gear and ST 2110-30 Level C send.
    CHECK(rtpPacketBytes(64, 3, 6) == 1164);
    CHECK(fits(64, 3, 6));
    CHECK(maxChannelsPerPacket(3, 6) == 81);
    // At 1 ms the same stream is 9228 bytes: over a 1500-byte frame, and
    // over a 9000-byte jumbo frame's 8972 as well.
    CHECK(rtpPacketBytes(64, 3, 48) == 9228);
    CHECK(!fits(64, 3, 48));
    // Seven samples per channel is the most 64 channels of L24 get, which
    // at 48 kHz is under 146 us: the 125 us step is the one that fits.
    CHECK(maxFramesPerPacket(64, 3) == 7);
}

TEST_CASE("At 96 kHz a millisecond holds five channels, not eight") {
    // Eight channels of L24 at 96 samples are 2316 bytes: what the
    // transmitter used to send, IP-fragmented, and the receiver dropped.
    CHECK(rtpPacketBytes(8, 3, 96) == 2316);
    CHECK(!fits(8, 3, 96));
    CHECK(maxChannelsPerPacket(3, 96) == 5);
    CHECK(fits(5, 3, 96));
}

TEST_CASE("Degenerate inputs give zero rather than a division by zero") {
    CHECK(maxChannelsPerPacket(0, 48) == 0);
    CHECK(maxChannelsPerPacket(3, 0) == 0);
    CHECK(maxFramesPerPacket(0, 3) == 0);
    CHECK(maxFramesPerPacket(8, 0) == 0);
    // A single sample of one channel always fits.
    CHECK(fits(1, 3, 1));
}

TEST_CASE("Usable in a constant expression") {
    static_assert(fits(8, 3, 48), "AES67's baseline flow fits");
    static_assert(!fits(64, 3, 48), "and 64 channels at 1 ms does not");
    static_assert(maxChannelsPerPacket(3, 48) == 10, "");
    CHECK(true);
}
