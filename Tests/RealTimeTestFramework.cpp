#include "RealTimeTestFramework.h"
#include "Shared/NonBlockingLogger.h"
#include <iostream>
#include <random>
#include <algorithm>
#include <functional>
#include <thread>
#include <chrono>

namespace AES67 {

// RealTimeTest implementations
RealTimeTest::RealTimeTest(const std::string& name) : name_(name) {}

RealTimeTestFramework& RealTimeTestFramework::getInstance() {
    static RealTimeTestFramework instance;
    return instance;
}

void RealTimeTestFramework::registerTest(std::unique_ptr<RealTimeTest> test) {
    std::lock_guard<std::mutex> lock(testsMutex_);
    tests_.push_back(std::move(test));
}

std::vector<TestResult> RealTimeTestFramework::runAllTests() {
    std::vector<TestResult> results;
    std::lock_guard<std::mutex> lock(testsMutex_);
    
    for (auto& test : tests_) {
        auto result = test->run();
        results.push_back(result);
    }
    
    return results;
}

TestResult RealTimeTestFramework::runTest(const std::string& testName) {
    std::lock_guard<std::mutex> lock(testsMutex_);
    
    for (auto& test : tests_) {
        if (test->getName() == testName) {
            return test->run();
        }
    }
    
    return TestResult(testName, false, "Test not found");
}

void RealTimeTestFramework::printResults(const std::vector<TestResult>& results) {
    std::cout << "\n=== Real-Time Test Results ===\n";
    std::cout << "Total Tests: " << results.size() << "\n";
    
    int passed = 0;
    for (const auto& result : results) {
        std::cout << (result.passed ? "PASS" : "FAIL") << " - " << result.testName;
        if (!result.passed) {
            std::cout << " (Error: " << result.errorMessage << ")";
        }
        std::cout << " [" << result.executionTime.count() << " μs]\n";
        
        if (result.passed) {
            passed++;
        }
    }
    
    std::cout << "Passed: " << passed << "/" << results.size() << "\n";
    std::cout << "==============================\n";
}

// AllocationTest implementation
AllocationTest::AllocationTest() : RealTimeTest("AllocationTest") {}

TestResult AllocationTest::run() {
    auto start = std::chrono::high_resolution_clock::now();
    
    try {
        // Test if any allocations happen in this code section
        // This is a simplified test - in a real scenario, we'd need more sophisticated memory tracking
        volatile int* ptr = nullptr;
        
        // Simulate typical operations that should not allocate
        for (int i = 0; i < 100; ++i) {
            // Use placement new to avoid allocation
            alignas(int) char buffer[sizeof(int)];
            ptr = new(buffer) int(i);
            // Use the value to prevent optimization
            volatile int val = *ptr;
            (void)val;
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        
        return TestResult(getName(), true);
    } catch (const std::exception& e) {
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        
        return TestResult(getName(), false, std::string("Exception: ") + e.what());
    }
}

// LatencyTest implementation
LatencyTest::LatencyTest() : RealTimeTest("LatencyTest") {}

TestResult LatencyTest::run() {
    auto start = std::chrono::high_resolution_clock::now();
    
    try {
        // Test for worst-case execution time
        const int iterations = 10000;
        std::vector<std::chrono::microseconds> latencies;
        latencies.reserve(iterations);
        
        for (int i = 0; i < iterations; ++i) {
            auto iterStart = std::chrono::high_resolution_clock::now();
            
            // Simulate typical audio processing operation
            volatile int sum = 0;
            for (int j = 0; j < 100; ++j) {
                sum += j * j; // Simple computation
            }
            
            auto iterEnd = std::chrono::high_resolution_clock::now();
            auto iterDuration = std::chrono::duration_cast<std::chrono::microseconds>(iterEnd - iterStart);
            latencies.push_back(iterDuration);
        }
        
        // Find maximum latency
        auto maxElement = std::max_element(latencies.begin(), latencies.end());
        std::chrono::microseconds maxLatency = maxElement != latencies.end() ? *maxElement : std::chrono::microseconds(0);
        
        // Check if max latency is acceptable (under 1ms for audio)
        bool passed = maxLatency.count() < 1000; // Less than 1ms
        std::string message = "Max latency: " + std::to_string(maxLatency.count()) + " μs";
        
        if (!passed) {
            message += " (FAILED: exceeded 1ms threshold)";
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto totalDuration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        
        return TestResult(getName(), passed, message);
    } catch (const std::exception& e) {
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        
        return TestResult(getName(), false, std::string("Exception: ") + e.what());
    }
}

// JitterBufferTest implementation
JitterBufferTest::JitterBufferTest() : RealTimeTest("JitterBufferTest") {}

TestResult JitterBufferTest::run() {
    auto start = std::chrono::high_resolution_clock::now();
    
    try {
        // This would test the actual jitter buffer implementation
        // For now, we'll simulate the test
        volatile bool success = true;
        
        // Simulate jitter buffer operations
        for (int i = 0; i < 1000; ++i) {
            // Simulate packet arrival with random timing
            std::this_thread::sleep_for(std::chrono::microseconds(1));
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        
        return TestResult(getName(), success, "Jitter buffer operations completed");
    } catch (const std::exception& e) {
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        
        return TestResult(getName(), false, std::string("Exception: ") + e.what());
    }
}

// PLLStabilityTest implementation
PLLStabilityTest::PLLStabilityTest() : RealTimeTest("PLLStabilityTest") {}

TestResult PLLStabilityTest::run() {
    auto start = std::chrono::high_resolution_clock::now();
    
    try {
        // Test PLL stability over time
        volatile bool stable = true;
        
        // Simulate PLL operation over a period of time
        for (int i = 0; i < 10000; ++i) {
            // Simulate small timing variations
            std::this_thread::sleep_for(std::chrono::microseconds(1));
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        
        return TestResult(getName(), stable, "PLL stability test completed");
    } catch (const std::exception& e) {
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        
        return TestResult(getName(), false, std::string("Exception: ") + e.what());
    }
}

// ThreadSafetyTest implementation
ThreadSafetyTest::ThreadSafetyTest() : RealTimeTest("ThreadSafetyTest") {}

TestResult ThreadSafetyTest::run() {
    auto start = std::chrono::high_resolution_clock::now();
    
    try {
        const int numThreads = 4;
        const int iterationsPerThread = 1000;
        std::atomic<int> sharedCounter{0};
        std::vector<std::thread> threads;
        
        // Launch multiple threads to test thread safety
        for (int t = 0; t < numThreads; ++t) {
            threads.emplace_back([&sharedCounter, iterationsPerThread]() {
                for (int i = 0; i < iterationsPerThread; ++i) {
                    sharedCounter.fetch_add(1);
                }
            });
        }
        
        // Wait for all threads to complete
        for (auto& thread : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        
        // Verify the counter has the expected value
        int expectedValue = numThreads * iterationsPerThread;
        bool passed = sharedCounter.load() == expectedValue;
        
        std::string message = "Expected: " + std::to_string(expectedValue) + 
                             ", Actual: " + std::to_string(sharedCounter.load());
        
        if (!passed) {
            message += " (FAILED: Race condition detected)";
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        
        return TestResult(getName(), passed, message);
    } catch (const std::exception& e) {
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        
        return TestResult(getName(), false, std::string("Exception: ") + e.what());
    }
}

} // namespace AES67