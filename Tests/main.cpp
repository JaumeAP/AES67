#include "RealTimeTestFramework.h"
#include "Shared/NonBlockingLogger.h"
#include <iostream>

int main() {
    // Initialize the logger
    AES67::g_logger = std::make_unique<AES67::NonBlockingLogger>("/tmp/aes67_test.log");
    
    // Create and register tests
    AES67::RealTimeTestFramework& framework = AES67::RealTimeTestFramework::getInstance();
    
    framework.registerTest(std::make_unique<AES67::AllocationTest>());
    framework.registerTest(std::make_unique<AES67::LatencyTest>());
    framework.registerTest(std::make_unique<AES67::JitterBufferTest>());
    framework.registerTest(std::make_unique<AES67::PLLStabilityTest>());
    framework.registerTest(std::make_unique<AES67::ThreadSafetyTest>());
    
    std::cout << "Running AES67 macOS Driver Real-Time Tests...\n";
    
    // Run all tests
    auto results = framework.runAllTests();
    
    // Print results
    framework.printResults(results);
    
    // Log summary
    int passed = 0;
    for (const auto& result : results) {
        if (result.passed) {
            passed++;
        }
    }
    
    std::string summary = "Test Summary: " + std::to_string(passed) + "/" + 
                         std::to_string(results.size()) + " tests passed";
    LOG_INFO(summary);
    
    std::cout << "Tests completed. Check /tmp/aes67_test.log for detailed logs.\n";
    
    return 0;
}