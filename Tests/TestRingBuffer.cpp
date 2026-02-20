//
// TestRingBuffer.cpp
// AES67 macOS Driver - Build #6
// Unit tests for lock-free SPSC ring buffer
//

#include "../Shared/RingBuffer.hpp"
#include <iostream>
#include <cassert>
#include <thread>
#include <chrono>
#include <vector>
#include <numeric>

using namespace AES67;

// Test result counter
static int testsPassed = 0;
static int testsFailed = 0;

#define TEST_ASSERT(condition, message) \
    if (!(condition)) { \
        std::cerr << "FAIL: " << message << std::endl; \
        testsFailed++; \
        return false; \
    } else { \
        testsPassed++; \
    }

//
// Basic Functionality Tests
//

bool testBasicWriteRead() {
    std::cout << "Test: Basic write/read... ";

    SPSCRingBuffer<float> buffer(64);

    // Write single sample
    float writeData = 42.0f;
    size_t written = buffer.write(&writeData, 1);
    TEST_ASSERT(written == 1, "Should write 1 sample");

    // Read single sample
    float readData = 0.0f;
    size_t read = buffer.read(&readData, 1);
    TEST_ASSERT(read == 1, "Should read 1 sample");
    TEST_ASSERT(readData == 42.0f, "Read data should match written data");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testBatchWriteRead() {
    std::cout << "Test: Batch write/read... ";

    SPSCRingBuffer<float> buffer(128);

    // Write batch of 64 samples
    float writeData[64];
    for (int i = 0; i < 64; ++i) {
        writeData[i] = static_cast<float>(i);
    }

    size_t written = buffer.write(writeData, 64);
    TEST_ASSERT(written == 64, "Should write all 64 samples");

    // Read batch
    float readData[64];
    size_t read = buffer.read(readData, 64);
    TEST_ASSERT(read == 64, "Should read all 64 samples");

    // Verify data integrity
    for (int i = 0; i < 64; ++i) {
        TEST_ASSERT(readData[i] == writeData[i], "Data integrity check");
    }

    std::cout << "PASS" << std::endl;
    return true;
}

bool testBufferWrapAround() {
    std::cout << "Test: Buffer wrap-around... ";

    SPSCRingBuffer<float> buffer(64);

    // Fill buffer almost completely
    float writeData[60];
    for (int i = 0; i < 60; ++i) {
        writeData[i] = static_cast<float>(i);
    }
    buffer.write(writeData, 60);

    // Read half
    float readData[30];
    buffer.read(readData, 30);

    // Write more (will wrap around)
    float moreData[40];
    for (int i = 0; i < 40; ++i) {
        moreData[i] = static_cast<float>(100 + i);
    }
    size_t written = buffer.write(moreData, 40);
    TEST_ASSERT(written == 40, "Should handle wrap-around write");

    // Read remaining from first batch
    buffer.read(readData, 30);

    // Read wrapped data
    float wrappedRead[40];
    size_t read = buffer.read(wrappedRead, 40);
    TEST_ASSERT(read == 40, "Should read wrapped data");

    // Verify wrapped data
    for (int i = 0; i < 40; ++i) {
        TEST_ASSERT(wrappedRead[i] == moreData[i], "Wrapped data integrity");
    }

    std::cout << "PASS" << std::endl;
    return true;
}

bool testBufferFull() {
    std::cout << "Test: Buffer full condition... ";

    SPSCRingBuffer<float> buffer(64);

    // Fill buffer completely (capacity - 1)
    float writeData[63];
    for (int i = 0; i < 63; ++i) {
        writeData[i] = static_cast<float>(i);
    }

    size_t written = buffer.write(writeData, 63);
    TEST_ASSERT(written == 63, "Should fill buffer");
    TEST_ASSERT(buffer.isFull(), "Buffer should be full");

    // Try to write more - should fail
    float moreData = 999.0f;
    written = buffer.write(&moreData, 1);
    TEST_ASSERT(written == 0, "Should not write when full");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testBufferEmpty() {
    std::cout << "Test: Buffer empty condition... ";

    SPSCRingBuffer<float> buffer(64);

    TEST_ASSERT(buffer.isEmpty(), "New buffer should be empty");

    // Try to read from empty buffer
    float readData;
    size_t read = buffer.read(&readData, 1);
    TEST_ASSERT(read == 0, "Should not read when empty");

    // Write and read
    float writeData = 42.0f;
    buffer.write(&writeData, 1);
    buffer.read(&readData, 1);

    TEST_ASSERT(buffer.isEmpty(), "Buffer should be empty after read");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testAvailable() {
    std::cout << "Test: Available space calculation... ";

    SPSCRingBuffer<float> buffer(64);

    TEST_ASSERT(buffer.available() == 0, "Empty buffer has 0 available");
    TEST_ASSERT(buffer.availableWrite() == 63, "Empty buffer has capacity-1 writable");

    // Write 32 samples
    float writeData[32];
    buffer.write(writeData, 32);

    TEST_ASSERT(buffer.available() == 32, "Should have 32 samples available");
    TEST_ASSERT(buffer.availableWrite() == 31, "Should have 31 samples writable");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testReset() {
    std::cout << "Test: Buffer reset... ";

    SPSCRingBuffer<float> buffer(64);

    // Fill buffer
    float writeData[32];
    buffer.write(writeData, 32);

    // Reset
    buffer.reset();

    TEST_ASSERT(buffer.isEmpty(), "Reset buffer should be empty");
    TEST_ASSERT(buffer.available() == 0, "Reset buffer has 0 available");

    std::cout << "PASS" << std::endl;
    return true;
}

//
// Performance Tests
//

bool testBatchPerformance() {
    std::cout << "Test: Batch processing performance... ";

    SPSCRingBuffer<float> buffer(512);

    constexpr size_t kNumIterations = 10000;
    constexpr size_t kBatchSize = 64;

    float writeData[kBatchSize];
    float readData[kBatchSize];

    for (size_t i = 0; i < kBatchSize; ++i) {
        writeData[i] = static_cast<float>(i);
    }

    auto start = std::chrono::high_resolution_clock::now();

    // Batch processing (what we do now)
    for (size_t i = 0; i < kNumIterations; ++i) {
        buffer.write(writeData, kBatchSize);
        buffer.read(readData, kBatchSize);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto batchDuration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    // Compare to per-sample processing (old way)
    buffer.reset();
    start = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < kNumIterations; ++i) {
        for (size_t j = 0; j < kBatchSize; ++j) {
            buffer.write(&writeData[j], 1);
        }
        for (size_t j = 0; j < kBatchSize; ++j) {
            buffer.read(&readData[j], 1);
        }
    }

    end = std::chrono::high_resolution_clock::now();
    auto singleDuration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double speedup = static_cast<double>(singleDuration.count()) / batchDuration.count();

    std::cout << "PASS (Speedup: " << speedup << "x)" << std::endl;
    std::cout << "  Batch:  " << batchDuration.count() << " μs" << std::endl;
    std::cout << "  Single: " << singleDuration.count() << " μs" << std::endl;

    TEST_ASSERT(speedup > 1.5, "Batch should be at least 1.5x faster");

    return true;
}

bool testThreadSafety() {
    std::cout << "Test: Thread safety (SPSC)... ";

    SPSCRingBuffer<float> buffer(1024);

    constexpr size_t kNumSamples = 100000;
    std::atomic<bool> producerDone{false};
    std::atomic<size_t> samplesWritten{0};
    std::atomic<size_t> samplesRead{0};

    // Producer thread
    std::thread producer([&]() {
        for (size_t i = 0; i < kNumSamples; ++i) {
            float value = static_cast<float>(i);
            while (buffer.write(&value, 1) != 1) {
                std::this_thread::yield();
            }
            samplesWritten++;
        }
        producerDone = true;
    });

    // Consumer thread
    std::thread consumer([&]() -> void {
        float value;
        while (!producerDone || !buffer.isEmpty()) {
            if (buffer.read(&value, 1) == 1) {
                if (value != static_cast<float>(samplesRead.load())) {
                    std::cerr << "FAIL: " << "Data should be in order" << std::endl;
                    testsFailed++;
                    return;
                } else {
                    testsPassed++;
                }
                samplesRead++;
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    TEST_ASSERT(samplesWritten == kNumSamples, "All samples should be written");
    TEST_ASSERT(samplesRead == kNumSamples, "All samples should be read");

    std::cout << "PASS (" << kNumSamples << " samples)" << std::endl;
    return true;
}

//
// Edge Cases
//

bool testZeroSizeOperations() {
    std::cout << "Test: Zero-size operations... ";

    SPSCRingBuffer<float> buffer(64);

    float data[1];

    size_t written = buffer.write(data, 0);
    TEST_ASSERT(written == 0, "Write 0 should return 0");

    size_t read = buffer.read(data, 0);
    TEST_ASSERT(read == 0, "Read 0 should return 0");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testPartialWrites() {
    std::cout << "Test: Partial writes when nearly full... ";

    SPSCRingBuffer<float> buffer(64);

    // Fill most of buffer
    float writeData[60];
    buffer.write(writeData, 60);

    // Try to write more than available
    float moreData[10];
    size_t written = buffer.write(moreData, 10);

    TEST_ASSERT(written == 3, "Should write only available space (63-60=3)");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testPartialReads() {
    std::cout << "Test: Partial reads when nearly empty... ";

    SPSCRingBuffer<float> buffer(64);

    // Write 5 samples
    float writeData[5];
    buffer.write(writeData, 5);

    // Try to read more than available
    float readData[10];
    size_t read = buffer.read(readData, 10);

    TEST_ASSERT(read == 5, "Should read only available data");

    std::cout << "PASS" << std::endl;
    return true;
}

//
// Main Test Runner
//

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "AES67 Ring Buffer Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;

    std::cout << "Basic Functionality Tests:" << std::endl;
    std::cout << "-------------------------" << std::endl;
    testBasicWriteRead();
    testBatchWriteRead();
    testBufferWrapAround();
    testBufferFull();
    testBufferEmpty();
    testAvailable();
    testReset();
    std::cout << std::endl;

    std::cout << "Performance Tests:" << std::endl;
    std::cout << "-----------------" << std::endl;
    testBatchPerformance();
    testThreadSafety();
    std::cout << std::endl;

    std::cout << "Edge Cases:" << std::endl;
    std::cout << "-----------" << std::endl;
    testZeroSizeOperations();
    testPartialWrites();
    testPartialReads();
    std::cout << std::endl;

    std::cout << "========================================" << std::endl;
    std::cout << "Test Results:" << std::endl;
    std::cout << "  Passed: " << testsPassed << std::endl;
    std::cout << "  Failed: " << testsFailed << std::endl;
    std::cout << "========================================" << std::endl;

    return testsFailed == 0 ? 0 : 1;
}
