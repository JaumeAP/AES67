//
// test_jitter_buffer_integration.cpp
// Test for jitter buffer integration in RTP receiver
//

#include "../NetworkEngine/RTP/LockFreeCircularJitterBuffer.h"
#include <iostream>
#include <cassert>
#include <cstring>

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
    assert(buffer.addPacket(packet1, sizeof(packet1), 100, 1000000));
    assert(buffer.addPacket(packet2, sizeof(packet2), 101, 2000000));
    assert(buffer.addPacket(packet3, sizeof(packet3), 102, 3000000));

    // Read packets back
    uint8_t output[100];
    size_t outputLen;
    uint64_t presentationTime;

    assert(buffer.getNextPacket(output, sizeof(output), outputLen, presentationTime, 100));
    assert(outputLen == 100);
    assert(presentationTime == 1000000);
    assert(output[0] == 0xAA);

    assert(buffer.getNextPacket(output, sizeof(output), outputLen, presentationTime, 101));
    assert(outputLen == 100);
    assert(presentationTime == 2000000);
    assert(output[0] == 0xBB);

    assert(buffer.getNextPacket(output, sizeof(output), outputLen, presentationTime, 102));
    assert(outputLen == 100);
    assert(presentationTime == 3000000);
    assert(output[0] == 0xCC);

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
    assert(buffer.addPacket(packet3, sizeof(packet3), 102, 3000000));
    assert(buffer.addPacket(packet1, sizeof(packet1), 100, 1000000));
    assert(buffer.addPacket(packet2, sizeof(packet2), 101, 2000000));

    // Read packets back in correct order
    uint8_t output[100];
    size_t outputLen;
    uint64_t presentationTime;

    assert(buffer.getNextPacket(output, sizeof(output), outputLen, presentationTime, 100));
    assert(output[0] == 0xAA);

    assert(buffer.getNextPacket(output, sizeof(output), outputLen, presentationTime, 101));
    assert(output[0] == 0xBB);

    assert(buffer.getNextPacket(output, sizeof(output), outputLen, presentationTime, 102));
    assert(output[0] == 0xCC);

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

    assert(buffer.addPacket(packet1, sizeof(packet1), seq1, 1000000));
    assert(buffer.addPacket(packet2, sizeof(packet2), seq2, 2000000));

    // Read packets back
    uint8_t output[100];
    size_t outputLen;
    uint64_t presentationTime;

    assert(buffer.getNextPacket(output, sizeof(output), outputLen, presentationTime, seq1));
    assert(output[0] == 0xAA);

    assert(buffer.getNextPacket(output, sizeof(output), outputLen, presentationTime, seq2));
    assert(output[0] == 0xBB);

    std::cout << "✓ Sequence wraparound test passed\n";
}

void test_buffer_full() {
    std::cout << "Testing buffer full condition...\n";

    LockFreeCircularJitterBuffer buffer;

    uint8_t packet[100];
    std::memset(packet, 0x55, sizeof(packet));

    // Fill the buffer
    size_t successCount = 0;
    for (size_t i = 0; i < LockFreeCircularJitterBuffer::BUFFER_SIZE + 10; i++) {
        if (buffer.addPacket(packet, sizeof(packet), i, i * 1000000)) {
            successCount++;
        }
    }

    // Should have succeeded for at most BUFFER_SIZE packets
    assert(successCount <= LockFreeCircularJitterBuffer::BUFFER_SIZE);

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
