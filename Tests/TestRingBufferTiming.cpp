//
// TestRingBufferTiming.cpp
// AES67 macOS Driver
// Wall-clock and concurrency tests for the lock-free SPSC ring buffer.
//
// Split out of TestRingBuffer.cpp so the deterministic ring-buffer cases can
// run in the pre-push gate. These two assert on a measured speed ratio and on
// two threads making progress against each other, both of which depend on
// machine load -- hence the `timing` label in Tests/CMakeLists.txt and the
// exclusion from the gate. Run them deliberately: `ctest -L timing`.
//

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "Shared/RingBuffer.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <numeric>

using namespace AES67;



TEST_CASE("Batch Performance") {
    std::cout << "Test: Batch processing performance... ";

    SPSCRingBuffer<float> buffer(512);

    constexpr size_t kNumIterations = 10000;
    constexpr size_t kBatchSize = 64;

    float writeData[kBatchSize];
    float readData[kBatchSize];

    for (size_t i = 0; i < kBatchSize; ++i) {
        writeData[i] = static_cast<float>(i);
    }

    // Consuming what was read is load-bearing, not cosmetic. Without it neither
    // loop below has an observable effect, and at -O3 -- the configuration this
    // is built in -- the optimiser deleted both: the test measured 0 us for
    // each, computed 0/0, and asserted on a NaN. It sat inside the suite the
    // gate excluded, so that went unnoticed. The sink is volatile so the reads
    // must actually happen.
    volatile float sink = 0.0f;

    auto start = std::chrono::high_resolution_clock::now();

    // Batch processing (what we do now)
    for (size_t i = 0; i < kNumIterations; ++i) {
        buffer.write(writeData, kBatchSize);
        buffer.read(readData, kBatchSize);
        sink = sink + readData[kBatchSize - 1];
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto batchNs = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

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
        sink = sink + readData[kBatchSize - 1];
    }

    end = std::chrono::high_resolution_clock::now();
    auto singleNs = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    std::cout << std::endl;
    std::cout << "  Batch:  " << batchNs << " ns" << std::endl;
    std::cout << "  Single: " << singleNs << " ns" << std::endl;

    // A zero here means the work vanished again, not that it was infinitely
    // fast. Catch it before dividing, or the ratio is a NaN that fails with a
    // misleading message about batching being slow.
    CHECK((batchNs > 0 && singleNs > 0));

    double speedup = static_cast<double>(singleNs) / static_cast<double>(batchNs);
    std::cout << "  Speedup: " << speedup << "x" << std::endl;

    CHECK(speedup > 1.5);
}

TEST_CASE("Thread Safety") {
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

    std::atomic<bool> outOfOrder{false};

    // Consumer thread
    std::thread consumer([&]() -> void {
        float value;
        while (!producerDone || !buffer.isEmpty()) {
            if (buffer.read(&value, 1) == 1) {
                // An atomic flag rather than an assertion: this runs on the
                // consumer thread, and doctest's macros are not safe to call
                // from anywhere but the thread running the case. The flag is
                // checked after the join, where asserting is legal.
                if (value != static_cast<float>(samplesRead.load())) {
                    outOfOrder = true;
                    return;
                }
                samplesRead++;
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    CHECK(outOfOrder == false);
    CHECK(samplesWritten == kNumSamples);
    CHECK(samplesRead == kNumSamples);

    std::cout << "PASS (" << kNumSamples << " samples)" << std::endl;
}

//
// Edge Cases
//

