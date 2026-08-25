//
// test_jitter_buffer_integration.cpp
// Test for jitter buffer integration in RTP receiver
//

#include "../NetworkEngine/RTP/LockFreeCircularJitterBuffer.h"
#include <iostream>
#include <cassert>
#include <stdexcept>
#include <string>
#include <cstring>

// assert() expands to nothing under NDEBUG, and NDEBUG is exactly how these
// tests get built: the Release configuration the local gate uses compiles with
// -O3 -DNDEBUG. Every check in this file silently vanished and the binary
// exited 0 no matter what the code did. AES67_CHECK throws instead, which
// main() already catches and turns into a non-zero exit.
#define AES67_CHECK(cond)                                                     \
    do {                                                                      \
        if (!(cond)) {                                                        \
            throw std::runtime_error(std::string(__FILE__) + ":" +            \
                                     std::to_string(__LINE__) +               \
                                     ": check failed: " #cond);               \
        }                                                                     \
    } while (0)

using namespace AES67;

void test_basic_integration() {
    std::cout << "Testing jitter buffer basic integration...\n";

    LockFreeCircularJitterBuffer buffer;

    // Simulate RTP packets
    uint8_t packet1[100];
    uint8_t packet2[100];
    uint8_t packet3[100];

    std::memset(packet1, 0xAA, sizeof(packet1));
    std::memset(packet2, 0xBB, sizeof(packet2));
    std::memset(packet3, 0xCC, sizeof(packet3));

    // Add packets in order
    AES67_CHECK(buffer.addPacket(packet1, sizeof(packet1), 100, 1000000));
    AES67_CHECK(buffer.addPacket(packet2, sizeof(packet2), 101, 2000000));
    AES67_CHECK(buffer.addPacket(packet3, sizeof(packet3), 102, 3000000));

    // Read packets back
    uint8_t output[100];
    size_t outputLen;
    uint64_t presentationTime;

    AES67_CHECK(buffer.getNextPacket(output, sizeof(output), outputLen, presentationTime, 100));
    AES67_CHECK(outputLen == 100);
    AES67_CHECK(presentationTime == 1000000);
    AES67_CHECK(output[0] == 0xAA);

    AES67_CHECK(buffer.getNextPacket(output, sizeof(output), outputLen, presentationTime, 101));
    AES67_CHECK(outputLen == 100);
    AES67_CHECK(presentationTime == 2000000);
    AES67_CHECK(output[0] == 0xBB);

    AES67_CHECK(buffer.getNextPacket(output, sizeof(output), outputLen, presentationTime, 102));
    AES67_CHECK(outputLen == 100);
    AES67_CHECK(presentationTime == 3000000);
    AES67_CHECK(output[0] == 0xCC);

    std::cout << "✓ Basic integration test passed\n";
}

void test_out_of_order() {
    std::cout << "Testing out-of-order packet handling...\n";

    LockFreeCircularJitterBuffer buffer;

    uint8_t packet1[100];
    uint8_t packet2[100];
    uint8_t packet3[100];

    std::memset(packet1, 0xAA, sizeof(packet1));
    std::memset(packet2, 0xBB, sizeof(packet2));
    std::memset(packet3, 0xCC, sizeof(packet3));

    // Add packets OUT of order (3, 1, 2)
    AES67_CHECK(buffer.addPacket(packet3, sizeof(packet3), 102, 3000000));
    AES67_CHECK(buffer.addPacket(packet1, sizeof(packet1), 100, 1000000));
    AES67_CHECK(buffer.addPacket(packet2, sizeof(packet2), 101, 2000000));

    // Read packets back in correct order
    uint8_t output[100];
    size_t outputLen;
    uint64_t presentationTime;

    AES67_CHECK(buffer.getNextPacket(output, sizeof(output), outputLen, presentationTime, 100));
    AES67_CHECK(output[0] == 0xAA);

    AES67_CHECK(buffer.getNextPacket(output, sizeof(output), outputLen, presentationTime, 101));
    AES67_CHECK(output[0] == 0xBB);

    AES67_CHECK(buffer.getNextPacket(output, sizeof(output), outputLen, presentationTime, 102));
    AES67_CHECK(output[0] == 0xCC);

    std::cout << "✓ Out-of-order test passed\n";
}

void test_sequence_wraparound() {
    std::cout << "Testing 16-bit sequence number wraparound...\n";

    LockFreeCircularJitterBuffer buffer;

    uint8_t packet1[100];
    uint8_t packet2[100];

    std::memset(packet1, 0xAA, sizeof(packet1));
    std::memset(packet2, 0xBB, sizeof(packet2));

    // Add packets around 16-bit wraparound
    uint32_t seq1 = 0xFFFF;  // Last value before wraparound
    uint32_t seq2 = 0x0000;  // First value after wraparound

    AES67_CHECK(buffer.addPacket(packet1, sizeof(packet1), seq1, 1000000));
    AES67_CHECK(buffer.addPacket(packet2, sizeof(packet2), seq2, 2000000));

    // Read packets back
    uint8_t output[100];
    size_t outputLen;
    uint64_t presentationTime;

    AES67_CHECK(buffer.getNextPacket(output, sizeof(output), outputLen, presentationTime, seq1));
    AES67_CHECK(output[0] == 0xAA);

    AES67_CHECK(buffer.getNextPacket(output, sizeof(output), outputLen, presentationTime, seq2));
    AES67_CHECK(output[0] == 0xBB);

    std::cout << "✓ Sequence wraparound test passed\n";
}

void test_buffer_full() {
    std::cout << "Testing buffer full condition...\n";

    LockFreeCircularJitterBuffer buffer;

    uint8_t packet[100];
    std::memset(packet, 0x55, sizeof(packet));

    // Fill the buffer (use getMaxBufferSize() since buffer size is now configurable)
    const size_t maxSize = buffer.getMaxBufferSize();
    size_t successCount = 0;
    for (size_t i = 0; i < maxSize + 10; i++) {
        if (buffer.addPacket(packet, sizeof(packet), i, i * 1000000)) {
            successCount++;
        }
    }

    // Should have succeeded for at most maxSize packets
    AES67_CHECK(successCount <= maxSize);

    std::cout << "✓ Buffer full test passed (accepted " << successCount << " packets)\n";
}

int main() {
    std::cout << "=== Jitter Buffer Integration Tests ===\n\n";

    test_basic_integration();
    test_out_of_order();
    test_sequence_wraparound();
    test_buffer_full();

    std::cout << "\n=== All tests passed! ===\n";
    return 0;
}
