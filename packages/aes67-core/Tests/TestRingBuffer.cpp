//
// TestRingBuffer.cpp
// AES67 macOS Driver - Build #6
// Deterministic unit tests for the lock-free SPSC ring buffer.
//
// The wall-clock and two-thread cases live in TestRingBufferTiming.cpp: they
// assert on a speed ratio and on concurrent progress, which makes them
// sensitive to machine load, so they carry the `timing` label and stay out of
// the pre-push gate. Everything here is deterministic and always runs.
//

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "Shared/RingBuffer.hpp"
#include <iostream>
#include <cassert>
#include <vector>
#include <numeric>

using namespace AES67;

// Test result counter


//
// Basic Functionality Tests
//

TEST_CASE("Basic Write Read") {
    std::cout << "Test: Basic write/read... ";

    SPSCRingBuffer<float> buffer(64);

    // Write single sample
    float writeData = 42.0f;
    size_t written = buffer.write(&writeData, 1);
    CHECK(written == 1);

    // Read single sample
    float readData = 0.0f;
    size_t read = buffer.read(&readData, 1);
    CHECK(read == 1);
    CHECK(readData == 42.0f);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Batch Write Read") {
    std::cout << "Test: Batch write/read... ";

    SPSCRingBuffer<float> buffer(128);

    // Write batch of 64 samples
    float writeData[64];
    for (int i = 0; i < 64; ++i) {
        writeData[i] = static_cast<float>(i);
    }

    size_t written = buffer.write(writeData, 64);
    CHECK(written == 64);

    // Read batch
    float readData[64];
    size_t read = buffer.read(readData, 64);
    CHECK(read == 64);

    // Verify data integrity
    for (int i = 0; i < 64; ++i) {
        CHECK(readData[i] == writeData[i]);
    }

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Buffer Wrap Around") {
    std::cout << "Test: Buffer wrap-around... ";

    SPSCRingBuffer<float> buffer(64);

    // Write 50 samples (capacity is 64, so 14 slots remain)
    float writeData[50];
    for (int i = 0; i < 50; ++i) {
        writeData[i] = static_cast<float>(i);
    }
    buffer.write(writeData, 50);

    // Read 30, freeing space (20 items remain, 44 writable)
    float readData[30];
    buffer.read(readData, 30);

    // Write 40 more (will wrap around past end of internal buffer)
    float moreData[40];
    for (int i = 0; i < 40; ++i) {
        moreData[i] = static_cast<float>(100 + i);
    }
    size_t written = buffer.write(moreData, 40);
    CHECK(written == 40);

    // Read remaining 20 from first batch
    float remainder[20];
    size_t read = buffer.read(remainder, 20);
    CHECK(read == 20);

    // Read wrapped data
    float wrappedRead[40];
    read = buffer.read(wrappedRead, 40);
    CHECK(read == 40);

    // Verify wrapped data
    for (int i = 0; i < 40; ++i) {
        CHECK(wrappedRead[i] == moreData[i]);
    }

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Buffer Full") {
    std::cout << "Test: Buffer full condition... ";

    SPSCRingBuffer<float> buffer(64);

    // Fill buffer completely (full capacity)
    float writeData[64];
    for (int i = 0; i < 64; ++i) {
        writeData[i] = static_cast<float>(i);
    }

    size_t written = buffer.write(writeData, 64);
    CHECK(written == 64);
    CHECK(buffer.isFull());

    // Try to write more - should fail
    float moreData = 999.0f;
    written = buffer.write(&moreData, 1);
    CHECK(written == 0);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Buffer Empty") {
    std::cout << "Test: Buffer empty condition... ";

    SPSCRingBuffer<float> buffer(64);

    CHECK(buffer.isEmpty());

    // Try to read from empty buffer
    float readData;
    size_t read = buffer.read(&readData, 1);
    CHECK(read == 0);

    // Write and read
    float writeData = 42.0f;
    buffer.write(&writeData, 1);
    buffer.read(&readData, 1);

    CHECK(buffer.isEmpty());

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Available") {
    std::cout << "Test: Available space calculation... ";

    SPSCRingBuffer<float> buffer(64);

    CHECK(buffer.available() == 0);
    CHECK(buffer.availableWrite() == 64);

    // Write 32 samples
    float writeData[32];
    buffer.write(writeData, 32);

    CHECK(buffer.available() == 32);
    CHECK(buffer.availableWrite() == 32);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Reset") {
    std::cout << "Test: Buffer reset... ";

    SPSCRingBuffer<float> buffer(64);

    // Fill buffer
    float writeData[32];
    buffer.write(writeData, 32);

    // Reset
    buffer.reset();

    CHECK(buffer.isEmpty());
    CHECK(buffer.available() == 0);

    std::cout << "PASS" << std::endl;
}

//
// Performance Tests
//

TEST_CASE("Zero Size Operations") {
    std::cout << "Test: Zero-size operations... ";

    SPSCRingBuffer<float> buffer(64);

    float data[1];

    size_t written = buffer.write(data, 0);
    CHECK(written == 0);

    size_t read = buffer.read(data, 0);
    CHECK(read == 0);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Partial Writes") {
    std::cout << "Test: Partial writes when nearly full... ";

    SPSCRingBuffer<float> buffer(64);

    // Fill most of buffer
    float writeData[60];
    buffer.write(writeData, 60);

    // Try to write more than available
    float moreData[10];
    size_t written = buffer.write(moreData, 10);

    CHECK(written == 4);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Partial Reads") {
    std::cout << "Test: Partial reads when nearly empty... ";

    SPSCRingBuffer<float> buffer(64);

    // Write 5 samples
    float writeData[5];
    buffer.write(writeData, 5);

    // Try to read more than available
    float readData[10];
    size_t read = buffer.read(readData, 10);

    CHECK(read == 5);

    std::cout << "PASS" << std::endl;
}

//
// Main Test Runner
//

