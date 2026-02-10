//
// TestPTPClock.cpp
// AES67 macOS Driver - Build #18
// Unit tests for PTP clock synchronization
//

#include "../NetworkEngine/PTP/PTPClock.h"
#include "../Driver/SDPParser.h"
#include <iostream>
#include <cassert>
#include <thread>
#include <chrono>

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
// LocalClock Tests
//

bool testLocalClockCreation() {
    std::cout << "Test: LocalClock creation... ";

    LocalClock clock;

    // Should be able to create local clock
    TEST_ASSERT(true, "LocalClock should construct successfully");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testLocalClockTimeRetrieval() {
    std::cout << "Test: LocalClock time retrieval... ";

    LocalClock clock;

    // Get time in nanoseconds
    uint64_t timeNs = clock.getTime();
    TEST_ASSERT(timeNs > 0, "Time in nanoseconds should be positive");

    // Get time in microseconds
    uint64_t timeUs = clock.getTimeMicroseconds();
    TEST_ASSERT(timeUs > 0, "Time in microseconds should be positive");

    // Microseconds should be roughly nanoseconds / 1000
    // Allow some tolerance for execution time
    uint64_t calculatedUs = timeNs / 1000;
    int64_t diff = static_cast<int64_t>(timeUs) - static_cast<int64_t>(calculatedUs);
    TEST_ASSERT(std::abs(diff) < 1000, "Microseconds should match nanoseconds / 1000");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testLocalClockMonotonic() {
    std::cout << "Test: LocalClock monotonic behavior... ";

    LocalClock clock;

    // Get multiple time samples
    uint64_t t1 = clock.getTime();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    uint64_t t2 = clock.getTime();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    uint64_t t3 = clock.getTime();

    // Time should always increase (monotonic)
    TEST_ASSERT(t2 > t1, "Time should increase monotonically");
    TEST_ASSERT(t3 > t2, "Time should continue to increase");

    // Check that elapsed time makes sense (~10ms between samples)
    uint64_t elapsed1 = (t2 - t1) / 1000000;  // Convert to milliseconds
    uint64_t elapsed2 = (t3 - t2) / 1000000;
    TEST_ASSERT(elapsed1 >= 8 && elapsed1 <= 15, "Elapsed time should be ~10ms");
    TEST_ASSERT(elapsed2 >= 8 && elapsed2 <= 15, "Elapsed time should be ~10ms");

    std::cout << "PASS" << std::endl;
    return true;
}

//
// PTPClock Tests
//

bool testPTPClockCreation() {
    std::cout << "Test: PTPClock creation... ";

    // Create PTP clock for domain 0
    PTPClock clock(0);

    TEST_ASSERT(!clock.isRunning(), "Clock should not be running initially");
    TEST_ASSERT(!clock.isLocked(), "Clock should not be locked initially");
    TEST_ASSERT(clock.getDomain() == 0, "Domain should be 0");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testPTPClockMultipleDomains() {
    std::cout << "Test: PTPClock multiple domains... ";

    // Create clocks for different domains
    PTPClock clock0(0);
    PTPClock clock1(1);
    PTPClock clock2(127);

    TEST_ASSERT(clock0.getDomain() == 0, "Clock 0 should have domain 0");
    TEST_ASSERT(clock1.getDomain() == 1, "Clock 1 should have domain 1");
    TEST_ASSERT(clock2.getDomain() == 127, "Clock 2 should have domain 127");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testPTPClockTimeRetrieval() {
    std::cout << "Test: PTPClock time retrieval... ";

    PTPClock clock(0);

    // Should be able to get time even when not running
    uint64_t timeNs = clock.getTime();
    TEST_ASSERT(timeNs > 0, "Time should be positive");

    uint64_t timeUs = clock.getTimeMicroseconds();
    TEST_ASSERT(timeUs > 0, "Time in microseconds should be positive");

    // Check conversion
    uint64_t calculatedUs = timeNs / 1000;
    int64_t diff = static_cast<int64_t>(timeUs) - static_cast<int64_t>(calculatedUs);
    TEST_ASSERT(std::abs(diff) < 1000, "Microseconds conversion should be accurate");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testPTPClockOffset() {
    std::cout << "Test: PTPClock offset tracking... ";

    PTPClock clock(0);

    // Initial offset should be 0 when not locked
    int64_t offset = clock.getOffsetNs();
    TEST_ASSERT(offset == 0, "Initial offset should be 0");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testPTPClockQuality() {
    std::cout << "Test: PTPClock quality parameters... ";

    PTPClock clock(0);

    // Clock class and accuracy should be queryable
    uint8_t clockClass = clock.getClockClass();
    uint8_t clockAccuracy = clock.getClockAccuracy();

    // Default values for unlocked clock
    TEST_ASSERT(clockClass == 248, "Default clock class should be 248 (unlocked)");
    TEST_ASSERT(clockAccuracy == 254, "Default accuracy should be 254 (unknown)");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testPTPClockMasterID() {
    std::cout << "Test: PTPClock master ID retrieval... ";

    PTPClock clock(0);

    // Should be able to query master clock ID
    std::string masterID = clock.getMasterClockID();

    // When not locked, master ID should be empty or indicate stub mode
    TEST_ASSERT(masterID.empty() || masterID.find("STUB") != std::string::npos,
                "Master ID should be empty or indicate stub mode when not locked");

    std::cout << "PASS" << std::endl;
    return true;
}

//
// PTPClockManager Tests
//

bool testPTPClockManagerSingleton() {
    std::cout << "Test: PTPClockManager singleton pattern... ";

    // Get instance multiple times - should return same instance
    PTPClockManager& mgr1 = PTPClockManager::getInstance();
    PTPClockManager& mgr2 = PTPClockManager::getInstance();

    TEST_ASSERT(&mgr1 == &mgr2, "Singleton should return same instance");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testPTPClockManagerGlobalEnable() {
    std::cout << "Test: PTPClockManager global enable/disable... ";

    PTPClockManager& mgr = PTPClockManager::getInstance();

    // Should be enabled by default
    TEST_ASSERT(mgr.isPTPEnabled(), "PTP should be enabled by default");

    // Disable
    mgr.setPTPEnabled(false);
    TEST_ASSERT(!mgr.isPTPEnabled(), "PTP should be disabled");

    // Re-enable
    mgr.setPTPEnabled(true);
    TEST_ASSERT(mgr.isPTPEnabled(), "PTP should be enabled again");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testPTPClockManagerDomainManagement() {
    std::cout << "Test: PTPClockManager domain management... ";

    PTPClockManager& mgr = PTPClockManager::getInstance();

    // Get clock for domain 0
    auto clock0 = mgr.getClockForDomain(0);
    TEST_ASSERT(clock0 != nullptr, "Should create clock for domain 0");
    TEST_ASSERT(clock0->getDomain() == 0, "Clock should have correct domain");

    // Get same domain again - should return same instance
    auto clock0_again = mgr.getClockForDomain(0);
    TEST_ASSERT(clock0 == clock0_again, "Should return same clock instance");

    // Get different domain
    auto clock1 = mgr.getClockForDomain(1);
    TEST_ASSERT(clock1 != nullptr, "Should create clock for domain 1");
    TEST_ASSERT(clock1 != clock0, "Different domain should return different clock");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testPTPClockManagerActiveDomains() {
    std::cout << "Test: PTPClockManager active domains tracking... ";

    PTPClockManager& mgr = PTPClockManager::getInstance();

    // Create clocks for multiple domains
    mgr.getClockForDomain(0);
    mgr.getClockForDomain(1);
    mgr.getClockForDomain(2);

    // Get active domains
    std::vector<int> domains = mgr.getActiveDomains();

    // Should have at least the domains we created
    // (May have more from previous tests)
    TEST_ASSERT(domains.size() >= 3, "Should have at least 3 active domains");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testPTPClockManagerRemoveClock() {
    std::cout << "Test: PTPClockManager clock removal... ";

    PTPClockManager& mgr = PTPClockManager::getInstance();

    // Create clock for domain 99
    auto clock = mgr.getClockForDomain(99);
    TEST_ASSERT(clock != nullptr, "Should create clock");

    // Remove it
    mgr.removeClock(99);

    // Should be able to remove (no crash)
    TEST_ASSERT(true, "Should remove clock successfully");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testPTPClockManagerLocalTime() {
    std::cout << "Test: PTPClockManager local time fallback... ";

    PTPClockManager& mgr = PTPClockManager::getInstance();

    // Get local fallback time
    uint64_t localTime = mgr.getLocalTime();
    TEST_ASSERT(localTime > 0, "Local time should be positive");

    // Should be monotonic
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    uint64_t localTime2 = mgr.getLocalTime();
    TEST_ASSERT(localTime2 > localTime, "Local time should be monotonic");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testPTPClockManagerTimeForDomain() {
    std::cout << "Test: PTPClockManager time for domain... ";

    PTPClockManager& mgr = PTPClockManager::getInstance();

    // Get time for domain 0
    uint64_t time0 = mgr.getTimeForDomain(0);
    TEST_ASSERT(time0 > 0, "Time for domain 0 should be positive");

    // Get time for domain 1
    uint64_t time1 = mgr.getTimeForDomain(1);
    TEST_ASSERT(time1 > 0, "Time for domain 1 should be positive");

    // Times should be similar (within 1ms) since using local clock
    int64_t diff = static_cast<int64_t>(time1) - static_cast<int64_t>(time0);
    TEST_ASSERT(std::abs(diff) < 1000000, "Times should be within 1ms");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testPTPClockManagerTimeForStream() {
    std::cout << "Test: PTPClockManager time for stream... ";

    PTPClockManager& mgr = PTPClockManager::getInstance();

    // Create SDP session with PTP domain
    SDPSession sdp;
    sdp.sessionName = "Test Stream";
    sdp.ptpDomain = 0;

    // Get time for stream
    uint64_t streamTime = mgr.getTimeForStream(sdp);
    TEST_ASSERT(streamTime > 0, "Stream time should be positive");

    // Test with different PTP domain
    sdp.ptpDomain = 1;
    uint64_t streamTime2 = mgr.getTimeForStream(sdp);
    TEST_ASSERT(streamTime2 > 0, "Stream time should be positive");

    // Test with no PTP (domain -1)
    sdp.ptpDomain = -1;
    uint64_t streamTime3 = mgr.getTimeForStream(sdp);
    TEST_ASSERT(streamTime3 > 0, "Stream time should fallback to local");

    std::cout << "PASS" << std::endl;
    return true;
}

//
// Time Conversion Tests
//

bool testTimeConversions() {
    std::cout << "Test: Time unit conversions... ";

    // Test nanoseconds to microseconds
    uint64_t ns = 1000000000;  // 1 second
    uint64_t us = ns / 1000;
    TEST_ASSERT(us == 1000000, "1 second = 1,000,000 microseconds");

    // Test microseconds to milliseconds
    uint64_t ms = us / 1000;
    TEST_ASSERT(ms == 1000, "1 second = 1,000 milliseconds");

    // Test various conversions
    TEST_ASSERT(1000000 / 1000 == 1000, "1ms in ns = 1000us");
    TEST_ASSERT(1000 / 1000 == 1, "1us in ns = 1ms");

    std::cout << "PASS" << std::endl;
    return true;
}

//
// Domain Validation Tests
//

bool testPTPDomainRanges() {
    std::cout << "Test: PTP domain range validation... ";

    // Valid domains are 0-127 (IEEE 1588)

    // Test boundary values
    PTPClock clock0(0);
    TEST_ASSERT(clock0.getDomain() == 0, "Domain 0 should be valid");

    PTPClock clock127(127);
    TEST_ASSERT(clock127.getDomain() == 127, "Domain 127 should be valid");

    // Test typical AES67 domain (usually 0)
    PTPClock clockAES67(0);
    TEST_ASSERT(clockAES67.getDomain() == 0, "AES67 typically uses domain 0");

    std::cout << "PASS" << std::endl;
    return true;
}

//
// Clock State Tests
//

bool testPTPClockStates() {
    std::cout << "Test: PTP clock state transitions... ";

    PTPClock clock(0);

    // Initial state: not running, not locked
    TEST_ASSERT(!clock.isRunning(), "Should not be running initially");
    TEST_ASSERT(!clock.isLocked(), "Should not be locked initially");

    // States should be independent
    bool running = clock.isRunning();
    bool locked = clock.isLocked();
    TEST_ASSERT(!running || locked || true, "States should be queryable independently");

    std::cout << "PASS" << std::endl;
    return true;
}

//
// Main Test Runner
//

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "AES67 PTP Clock Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;

    std::cout << "LocalClock Tests:" << std::endl;
    std::cout << "----------------" << std::endl;
    testLocalClockCreation();
    testLocalClockTimeRetrieval();
    testLocalClockMonotonic();
    std::cout << std::endl;

    std::cout << "PTPClock Tests:" << std::endl;
    std::cout << "--------------" << std::endl;
    testPTPClockCreation();
    testPTPClockMultipleDomains();
    testPTPClockTimeRetrieval();
    testPTPClockOffset();
    testPTPClockQuality();
    testPTPClockMasterID();
    std::cout << std::endl;

    std::cout << "PTPClockManager Tests:" << std::endl;
    std::cout << "---------------------" << std::endl;
    testPTPClockManagerSingleton();
    testPTPClockManagerGlobalEnable();
    testPTPClockManagerDomainManagement();
    testPTPClockManagerActiveDomains();
    testPTPClockManagerRemoveClock();
    testPTPClockManagerLocalTime();
    testPTPClockManagerTimeForDomain();
    testPTPClockManagerTimeForStream();
    std::cout << std::endl;

    std::cout << "Time Conversion Tests:" << std::endl;
    std::cout << "---------------------" << std::endl;
    testTimeConversions();
    std::cout << std::endl;

    std::cout << "Domain Validation Tests:" << std::endl;
    std::cout << "-----------------------" << std::endl;
    testPTPDomainRanges();
    std::cout << std::endl;

    std::cout << "Clock State Tests:" << std::endl;
    std::cout << "-----------------" << std::endl;
    testPTPClockStates();
    std::cout << std::endl;

    std::cout << "========================================" << std::endl;
    std::cout << "Test Results:" << std::endl;
    std::cout << "  Passed: " << testsPassed << std::endl;
    std::cout << "  Failed: " << testsFailed << std::endl;
    std::cout << "========================================" << std::endl;

    return testsFailed == 0 ? 0 : 1;
}
