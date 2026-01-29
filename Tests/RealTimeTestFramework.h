#ifndef REALTIME_TEST_FRAMEWORK_H
#define REALTIME_TEST_FRAMEWORK_H

#include <chrono>
#include <vector>
#include <functional>
#include <string>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>

namespace AES67 {

struct TestResult {
    std::string testName;
    bool passed;
    std::string errorMessage;
    std::chrono::microseconds executionTime;
    
    TestResult(const std::string& name, bool pass, const std::string& error = "")
        : testName(name), passed(pass), errorMessage(error) {
        executionTime = std::chrono::microseconds(0);
    }
};

class RealTimeTest {
public:
    explicit RealTimeTest(const std::string& name);
    virtual ~RealTimeTest() = default;
    
    // Run the test and return results
    virtual TestResult run() = 0;
    
    // Get the test name
    const std::string& getName() const { return name_; }
    
protected:
    std::string name_;
};

class RealTimeTestFramework {
public:
    static RealTimeTestFramework& getInstance();
    
    // Register a test
    void registerTest(std::unique_ptr<RealTimeTest> test);
    
    // Run all registered tests
    std::vector<TestResult> runAllTests();
    
    // Run a specific test by name
    TestResult runTest(const std::string& testName);
    
    // Print test results
    void printResults(const std::vector<TestResult>& results);
    
private:
    RealTimeTestFramework() = default;
    ~RealTimeTestFramework() = default;
    
    std::vector<std::unique_ptr<RealTimeTest>> tests_;
    mutable std::mutex testsMutex_;
};

// Specific tests
class AllocationTest : public RealTimeTest {
public:
    AllocationTest();
    TestResult run() override;
};

class LatencyTest : public RealTimeTest {
public:
    LatencyTest();
    TestResult run() override;
};

class JitterBufferTest : public RealTimeTest {
public:
    JitterBufferTest();
    TestResult run() override;
};

class PLLStabilityTest : public RealTimeTest {
public:
    PLLStabilityTest();
    TestResult run() override;
};

class ThreadSafetyTest : public RealTimeTest {
public:
    ThreadSafetyTest();
    TestResult run() override;
};

// Utility functions for tests
namespace TestUtils {
    // Measure execution time of a function
    template<typename Func>
    std::chrono::microseconds measureExecutionTime(Func&& func) {
        auto start = std::chrono::high_resolution_clock::now();
        func();
        auto end = std::chrono::high_resolution_clock::now();
        return std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    }
    
    // Check if function executes within time limit (for real-time tests)
    template<typename Func>
    bool executesWithinLimit(Func&& func, std::chrono::microseconds limit) {
        auto duration = measureExecutionTime(func);
        return duration <= limit;
    }
}

} // namespace AES67

#endif // REALTIME_TEST_FRAMEWORK_H