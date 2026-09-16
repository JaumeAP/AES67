//
// TestLockFreeJitterBuffer.cpp
// AES67 core
//
// The jitter buffer the receive path reads through, which nothing had ever
// tested: no suite in this tree so much as named it. What covered it was
// whatever RTPReceiver happened to do with real packets on a real socket,
// which is to say nothing that runs in a gate.
//
// It is a slot-per-sequence-number circular buffer with an atomic state
// machine per slot -- EMPTY, WRITING, READY, READING -- written by the
// network thread and read by the audio thread. What these cases pin is the
// bookkeeping around that: which slot a sequence number lands in, when
// validPackets_ goes down, and what happens to a slot whose packet is not the
// one that was asked for.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/RTP/LockFreeCircularJitterBuffer.h"

#include <cstdint>
#include <numeric>
#include <vector>

using namespace AES67;

namespace {

std::vector<uint8_t> payload(size_t length, uint8_t first = 0) {
    std::vector<uint8_t> bytes(length);
    std::iota(bytes.begin(), bytes.end(), first);
    return bytes;
}

} // namespace

TEST_CASE("A depth outside the allowed range is clamped and rounded") {
    // The mask that indexes a slot is bufferSize_ - 1, so the size has to be
    // a power of two for it to be a modulo at all.
    CHECK(LockFreeCircularJitterBuffer(100).getMaxBufferSize() == 128);
    CHECK(LockFreeCircularJitterBuffer(128).getMaxBufferSize() == 128);

    CHECK(LockFreeCircularJitterBuffer(1).getMaxBufferSize() ==
          LockFreeCircularJitterBuffer::MIN_BUFFER_SIZE);
    CHECK(LockFreeCircularJitterBuffer(1u << 20).getMaxBufferSize() ==
          LockFreeCircularJitterBuffer::MAX_BUFFER_SIZE);
}

TEST_CASE("A packet put in comes back out once") {
    LockFreeCircularJitterBuffer buffer(64);
    const std::vector<uint8_t> sent = payload(240, 7);

    REQUIRE(buffer.addPacket(sent.data(), sent.size(), 42, 1'000'000));
    CHECK(buffer.getBufferedPacketCount() == 1);

    uint8_t out[1500] = {};
    size_t length = 0;
    uint64_t presentation = 0;
    REQUIRE(buffer.getNextPacket(out, sizeof(out), length, presentation, 42));

    CHECK(length == sent.size());
    CHECK(presentation == 1'000'000);
    CHECK(std::vector<uint8_t>(out, out + length) == sent);

    // The slot went back to EMPTY, so it is not there to be read twice.
    CHECK(buffer.getBufferedPacketCount() == 0);
    CHECK_FALSE(buffer.getNextPacket(out, sizeof(out), length, presentation, 42));
}

TEST_CASE("Asking for a sequence number nothing wrote answers no") {
    LockFreeCircularJitterBuffer buffer(64);

    uint8_t out[1500] = {};
    size_t length = 0;
    uint64_t presentation = 0;
    CHECK_FALSE(buffer.getNextPacket(out, sizeof(out), length, presentation, 1));
    CHECK(buffer.getBufferedPacketCount() == 0);
}

TEST_CASE("A slot holding a different packet is released and counted down") {
    // Sequence numbers that are a whole buffer apart share a slot. Asking for
    // one and finding the other is the wrap-around case, and the slot has to
    // come back to EMPTY with validPackets_ decremented -- a count that only
    // goes up is a buffer that reports itself full for ever.
    LockFreeCircularJitterBuffer buffer(64);
    const std::vector<uint8_t> sent = payload(100);

    REQUIRE(buffer.addPacket(sent.data(), sent.size(), 5, 0));
    CHECK(buffer.getBufferedPacketCount() == 1);

    uint8_t out[1500] = {};
    size_t length = 0;
    uint64_t presentation = 0;
    CHECK_FALSE(buffer.getNextPacket(out, sizeof(out), length, presentation, 5 + 64));
    CHECK(buffer.getBufferedPacketCount() == 0);
}

TEST_CASE("A packet that does not fit the caller's buffer is released too") {
    // Same bookkeeping, a different reason to refuse: the packet is the one
    // asked for and there is nowhere to put it.
    LockFreeCircularJitterBuffer buffer(64);
    const std::vector<uint8_t> sent = payload(300);

    REQUIRE(buffer.addPacket(sent.data(), sent.size(), 9, 0));

    uint8_t small[64] = {};
    size_t length = 0;
    uint64_t presentation = 0;
    CHECK_FALSE(buffer.getNextPacket(small, sizeof(small), length, presentation, 9));
    CHECK(buffer.getBufferedPacketCount() == 0);
}

TEST_CASE("The buffer keeps no idea of where the stream is up to") {
    // There were two readers, identical but for a line advancing a member
    // nothing read. What is left says only what the caller asks it: two reads
    // of the same sequence number, one after the other, are a read and then a
    // miss, and reading 11 does not make 12 appear.
    LockFreeCircularJitterBuffer buffer(64);
    const std::vector<uint8_t> sent = payload(80);

    REQUIRE(buffer.addPacket(sent.data(), sent.size(), 11, 0));
    REQUIRE(buffer.addPacket(sent.data(), sent.size(), 13, 0));

    uint8_t out[1500] = {};
    size_t length = 0;
    uint64_t presentation = 0;

    REQUIRE(buffer.getNextPacket(out, sizeof(out), length, presentation, 11));
    CHECK_FALSE(buffer.getNextPacket(out, sizeof(out), length, presentation, 11));

    // 12 never arrived, and nothing here pretends otherwise; 13 did, and is
    // still there to be asked for.
    CHECK_FALSE(buffer.getNextPacket(out, sizeof(out), length, presentation, 12));
    CHECK(buffer.getNextPacket(out, sizeof(out), length, presentation, 13));
}

TEST_CASE("Reset empties every slot and the counts with them") {
    LockFreeCircularJitterBuffer buffer(64);
    const std::vector<uint8_t> sent = payload(50);

    for (uint32_t sequence = 0; sequence < 10; ++sequence) {
        REQUIRE(buffer.addPacket(sent.data(), sent.size(), sequence, sequence));
    }
    CHECK(buffer.getBufferedPacketCount() == 10);

    buffer.reset();
    CHECK(buffer.getBufferedPacketCount() == 0);

    uint8_t out[1500] = {};
    size_t length = 0;
    uint64_t presentation = 0;
    CHECK_FALSE(buffer.getNextPacket(out, sizeof(out), length, presentation, 0));
}
