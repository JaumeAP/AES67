//
// TestReferenceMath.cpp
// AES67 Linux PTP daemon
// Where an edge falls with respect to a second, which is the one number the
// servo is steering to zero.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "ReferenceMath.h"

using namespace AES67::LinuxPtpd;

TEST_CASE("An edge on the second is at zero") {
    CHECK(offsetFromSecondBoundary(0) == 0);
    CHECK(offsetFromSecondBoundary(1000000000ULL) == 0);
    CHECK(offsetFromSecondBoundary(1757203200000000000ULL) == 0);
}

TEST_CASE("A late edge is positive and an early one negative") {
    // 40 ns after the second.
    CHECK(offsetFromSecondBoundary(1000000040ULL) == 40);
    // 2 ns before the next one, which is -2 and not 999999998: a reference
    // that ticks once a second says where the boundary is, not which second.
    CHECK(offsetFromSecondBoundary(1999999998ULL) == -2);
}

TEST_CASE("The fold happens at half a second, in both directions") {
    CHECK(offsetFromSecondBoundary(1499999999ULL) == 499999999);
    CHECK(offsetFromSecondBoundary(1500000000ULL) == -500000000);
    CHECK(offsetFromSecondBoundary(1500000001ULL) == -499999999);
}

TEST_CASE("It works in a constant expression") {
    // The property that keeps it out of the platform half: no allocation, no
    // library, nothing that a test on another machine would not have.
    static_assert(offsetFromSecondBoundary(1000000040ULL) == 40, "");
    static_assert(offsetFromSecondBoundary(1999999998ULL) == -2, "");
}
