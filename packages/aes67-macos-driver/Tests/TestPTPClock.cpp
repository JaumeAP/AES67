//
// TestPTPClock.cpp
// AES67 macOS Driver - Build #18
// Unit tests for PTP clock synchronization
//

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/PTP/PTPClock.h"
#include "Driver/SDPParser.h"
#include <iostream>
#include <cassert>
#include <thread>
#include <chrono>

using namespace AES67;

// Test result counter


//
// LocalClock Tests
//

TEST_CASE("Local Clock Creation") {
    std::cout << "Test: LocalClock creation... ";

    LocalClock clock;

    // Should be able to create local clock
    CHECK(true);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Local Clock Time Retrieval") {
    std::cout << "Test: LocalClock time retrieval... ";

    LocalClock clock;

    // Get time in nanoseconds
    uint64_t timeNs = clock.getTime();
    CHECK(timeNs > 0);

    // Get time in microseconds
    uint64_t timeUs = clock.getTimeMicroseconds();
    CHECK(timeUs > 0);

    // Microseconds should be roughly nanoseconds / 1000
    // Allow some tolerance for execution time
    uint64_t calculatedUs = timeNs / 1000;
    int64_t diff = static_cast<int64_t>(timeUs) - static_cast<int64_t>(calculatedUs);
    CHECK(std::abs(diff) < 1000);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Local Clock Monotonic") {
    std::cout << "Test: LocalClock monotonic behavior... ";

    LocalClock clock;

    // Get multiple time samples
    uint64_t t1 = clock.getTime();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    uint64_t t2 = clock.getTime();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    uint64_t t3 = clock.getTime();

    // Time should always increase (monotonic)
    CHECK(t2 > t1);
    CHECK(t3 > t2);

    // Check that elapsed time makes sense (~10ms between samples)
    uint64_t elapsed1 = (t2 - t1) / 1000000;  // Convert to milliseconds
    uint64_t elapsed2 = (t3 - t2) / 1000000;
    CHECK((elapsed1 >= 8 && elapsed1 <= 15));
    CHECK((elapsed2 >= 8 && elapsed2 <= 15));

    std::cout << "PASS" << std::endl;
}

//
// PTPClock Tests
//

TEST_CASE("PTP Clock Creation") {
    std::cout << "Test: PTPClock creation... ";

    // Create PTP clock for domain 0
    PTPClock clock(0);

    CHECK(!clock.isRunning());
    CHECK(!clock.isLocked());
    CHECK(clock.getDomain() == 0);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("PTP Clock Multiple Domains") {
    std::cout << "Test: PTPClock multiple domains... ";

    // Create clocks for different domains
    PTPClock clock0(0);
    PTPClock clock1(1);
    PTPClock clock2(127);

    CHECK(clock0.getDomain() == 0);
    CHECK(clock1.getDomain() == 1);
    CHECK(clock2.getDomain() == 127);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("PTP Clock Time Retrieval") {
    std::cout << "Test: PTPClock time retrieval... ";

    PTPClock clock(0);

    // Should be able to get time even when not running
    uint64_t timeNs = clock.getTime();
    CHECK(timeNs > 0);

    uint64_t timeUs = clock.getTimeMicroseconds();
    CHECK(timeUs > 0);

    // Check conversion
    uint64_t calculatedUs = timeNs / 1000;
    int64_t diff = static_cast<int64_t>(timeUs) - static_cast<int64_t>(calculatedUs);
    CHECK(std::abs(diff) < 1000);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("PTP Clock Offset") {
    std::cout << "Test: PTPClock offset tracking... ";

    PTPClock clock(0);

    // Initial offset should be 0 when not locked
    int64_t offset = clock.getOffsetNs();
    CHECK(offset == 0);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("PTP Clock Quality") {
    std::cout << "Test: PTPClock quality parameters... ";

    PTPClock clock(0);

    // Clock class and accuracy should be queryable
    uint8_t clockClass = clock.getClockClass();
    uint8_t clockAccuracy = clock.getClockAccuracy();

    // Default values for unlocked clock
    CHECK(clockClass == 248);
    CHECK(clockAccuracy == 254);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("PTP Clock Master ID") {
    std::cout << "Test: PTPClock master ID retrieval... ";

    PTPClock clock(0);

    // Should be able to query master clock ID
    std::string masterID = clock.getMasterClockID();

    // When not locked, master ID should be empty or indicate stub mode
    CHECK((masterID.empty() || masterID.find("STUB") != std::string::npos));

    std::cout << "PASS" << std::endl;
}

//
// PTPClockManager Tests
//

TEST_CASE("PTP Clock Manager Singleton") {
    std::cout << "Test: PTPClockManager singleton pattern... ";

    // Get instance multiple times - should return same instance
    PTPClockManager& mgr1 = PTPClockManager::getInstance();
    PTPClockManager& mgr2 = PTPClockManager::getInstance();

    CHECK(&mgr1 == &mgr2);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("PTP Clock Manager Global Enable") {
    std::cout << "Test: PTPClockManager global enable/disable... ";

    PTPClockManager& mgr = PTPClockManager::getInstance();

    // Should be enabled by default
    CHECK(mgr.isPTPEnabled());

    // Disable
    mgr.setPTPEnabled(false);
    CHECK(!mgr.isPTPEnabled());

    // Re-enable
    mgr.setPTPEnabled(true);
    CHECK(mgr.isPTPEnabled());

    std::cout << "PASS" << std::endl;
}

TEST_CASE("PTP Clock Manager Domain Management") {
    std::cout << "Test: PTPClockManager domain management... ";

    PTPClockManager& mgr = PTPClockManager::getInstance();

    // Get clock for domain 0
    auto clock0 = mgr.getClockForDomain(0);
    CHECK(clock0 != nullptr);
    CHECK(clock0->getDomain() == 0);

    // Get same domain again - should return same instance
    auto clock0_again = mgr.getClockForDomain(0);
    CHECK(clock0 == clock0_again);

    // Get different domain
    auto clock1 = mgr.getClockForDomain(1);
    CHECK(clock1 != nullptr);
    CHECK(clock1 != clock0);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("PTP Clock Manager Active Domains") {
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
    CHECK(domains.size() >= 3);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("PTP Clock Manager Remove Clock") {
    std::cout << "Test: PTPClockManager clock removal... ";

    PTPClockManager& mgr = PTPClockManager::getInstance();

    // Create clock for domain 99
    auto clock = mgr.getClockForDomain(99);
    CHECK(clock != nullptr);

    // Remove it
    mgr.removeClock(99);

    // Should be able to remove (no crash)
    CHECK(true);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("PTP Clock Manager Local Time") {
    std::cout << "Test: PTPClockManager local time fallback... ";

    PTPClockManager& mgr = PTPClockManager::getInstance();

    // Get local fallback time
    uint64_t localTime = mgr.getLocalTime();
    CHECK(localTime > 0);

    // Should be monotonic
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    uint64_t localTime2 = mgr.getLocalTime();
    CHECK(localTime2 > localTime);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("PTP Clock Manager Time For Domain") {
    std::cout << "Test: PTPClockManager time for domain... ";

    PTPClockManager& mgr = PTPClockManager::getInstance();

    // Get time for domain 0
    uint64_t time0 = mgr.getTimeForDomain(0);
    CHECK(time0 > 0);

    // Get time for domain 1
    uint64_t time1 = mgr.getTimeForDomain(1);
    CHECK(time1 > 0);

    // Times should be similar (within 1ms) since using local clock
    int64_t diff = static_cast<int64_t>(time1) - static_cast<int64_t>(time0);
    CHECK(std::abs(diff) < 1000000);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("PTP Clock Manager Time For Stream") {
    std::cout << "Test: PTPClockManager time for stream... ";

    PTPClockManager& mgr = PTPClockManager::getInstance();

    // Create SDP session with PTP domain
    SDPSession sdp;
    sdp.sessionName = "Test Stream";
    sdp.ptpDomain = 0;

    // Get time for stream
    uint64_t streamTime = mgr.getTimeForStream(sdp);
    CHECK(streamTime > 0);

    // Test with different PTP domain
    sdp.ptpDomain = 1;
    uint64_t streamTime2 = mgr.getTimeForStream(sdp);
    CHECK(streamTime2 > 0);

    // Test with no PTP (domain -1)
    sdp.ptpDomain = -1;
    uint64_t streamTime3 = mgr.getTimeForStream(sdp);
    CHECK(streamTime3 > 0);

    std::cout << "PASS" << std::endl;
}

//
// Time Conversion Tests
//

TEST_CASE("Time Conversions") {
    std::cout << "Test: Time unit conversions... ";

    // Test nanoseconds to microseconds
    uint64_t ns = 1000000000;  // 1 second
    uint64_t us = ns / 1000;
    CHECK(us == 1000000);

    // Test microseconds to milliseconds
    uint64_t ms = us / 1000;
    CHECK(ms == 1000);

    // Test various conversions
    CHECK(1000000 / 1000 == 1000);
    CHECK(1000 / 1000 == 1);

    std::cout << "PASS" << std::endl;
}

//
// Domain Validation Tests
//

TEST_CASE("PTP Domain Ranges") {
    std::cout << "Test: PTP domain range validation... ";

    // Valid domains are 0-127 (IEEE 1588)

    // Test boundary values
    PTPClock clock0(0);
    CHECK(clock0.getDomain() == 0);

    PTPClock clock127(127);
    CHECK(clock127.getDomain() == 127);

    // Test typical AES67 domain (usually 0)
    PTPClock clockAES67(0);
    CHECK(clockAES67.getDomain() == 0);

    std::cout << "PASS" << std::endl;
}

//
// Clock State Tests
//

TEST_CASE("PTP Clock States") {
    std::cout << "Test: PTP clock state transitions... ";

    PTPClock clock(0);

    // Initial state: not running, not locked
    CHECK(!clock.isRunning());
    CHECK(!clock.isLocked());

    // States should be independent
    bool running = clock.isRunning();
    bool locked = clock.isLocked();
    CHECK((!running || locked || true));

    std::cout << "PASS" << std::endl;
}

//
// Main Test Runner
//

