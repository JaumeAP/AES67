//
// TestAmplifierUnit.cpp
// AES67 macOS Driver
// Pins the chained-unit source-port offset: the offset for the selected unit
// is the sum of the PRECEDING units' 8-channel flows, and those units may be
// different sizes (16/24/32) — not (unitIndex-1) times one uniform width.
// Header-only (the helper is a static inline), no linkage.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/AmplifierUnitSettings.h"

#include <iostream>
#include <vector>

using namespace AES67;


using U = AmplifierUnitSettings;

TEST_CASE("With no chain sizes the offset is uniform") {
    std::cout << "Test: with no chain sizes, offset is (unitIndex-1)*ownFlows... ";
    // 32-channel driver -> 4 flows per unit. Empty chain -> uniform fallback.
    CHECK(U::flowOffsetForUnit({}, 32, 1) == 0);
    CHECK(U::flowOffsetForUnit({}, 32, 2) == 4);
    CHECK(U::flowOffsetForUnit({}, 32, 3) == 8);
    // 16-channel driver -> 2 flows per unit.
    CHECK(U::flowOffsetForUnit({}, 16, 2) == 2);
    std::cout << "PASS" << std::endl;
}

TEST_CASE("Mixed unit sizes sum the preceding flows") {
    std::cout << "Test: mixed unit sizes give per-unit flow offsets... ";
    // Chain: unit1=16 (2 flows), unit2=32 (4 flows), unit3=24 (3 flows).
    std::vector<uint32_t> chain = {16, 32, 24};
    CHECK(U::flowOffsetForUnit(chain, 32, 1) == 0);
    // Feeding unit 2: only unit1 (16 -> 2 flows) precedes it.
    CHECK(U::flowOffsetForUnit(chain, 32, 2) == 2);
    // Feeding unit 3: unit1 (2) + unit2 (4) = 6 flows precede.
    CHECK(U::flowOffsetForUnit(chain, 32, 3) == 6);

    // Another chain: 32, 16, 24.
    std::vector<uint32_t> chain2 = {32, 16, 24};
    CHECK(U::flowOffsetForUnit(chain2, 16, 2) == 4);
    CHECK(U::flowOffsetForUnit(chain2, 16, 3) == 6);

    // 24-channel first unit -> 3 flows (the user's 24 example).
    CHECK(U::flowOffsetForUnit({24}, 32, 2) == 3);
    std::cout << "PASS" << std::endl;
}

TEST_CASE("A partial or zeroed chain falls back") {
    std::cout << "Test: unspecified/zero positions fall back to the driver width... ";
    // Only unit1 known (16); unit2 unknown -> fallback 32 for position 2.
    std::vector<uint32_t> chain = {16};
    CHECK(U::flowOffsetForUnit(chain, 32, 2) == 2);
    // Feeding unit 3 with only unit1 known: unit1=16 (2), unit2 unknown->32 (4) = 6.
    CHECK(U::flowOffsetForUnit(chain, 32, 3) == 6);
    // A 0 entry is treated as unknown -> fallback.
    std::vector<uint32_t> withZero = {0, 24};
    CHECK(U::flowOffsetForUnit(withZero, 32, 2) == 4);
    std::cout << "PASS" << std::endl;
}

