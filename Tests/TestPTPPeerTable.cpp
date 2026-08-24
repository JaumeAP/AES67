//
// TestPTPPeerTable.cpp
// AES67 macOS Driver
// Pins the passive-PTP-peer aggregation core: role inference from message
// types, OUI extraction from a clock identity, dedup by identity, and
// timeout eviction. Socket-free, time-injected — no PTPPeerObserver, no
// network — exactly like TestStreamManager pins evaluateSinkFollow.
//
#include "../NetworkEngine/PTP/PTPPeerTable.h"

#include <array>
#include <iostream>

using namespace AES67;

static int passed = 0;
static int failed = 0;
#define CHECK(cond, msg) \
    if (!(cond)) { std::cerr << "FAIL: " << msg << std::endl; failed++; return false; } \
    else { passed++; }

using Clock = std::chrono::steady_clock;
using R = PTPPeerRole;

static std::array<uint8_t, 8> idWithOui(uint8_t a, uint8_t b, uint8_t c, uint8_t tail) {
    // A MAC-derived EUI-64: OUI, then FF FE, then the rest of the MAC.
    return {a, b, c, 0xFF, 0xFE, 0x00, 0x00, tail};
}

bool testRoleInference() {
    std::cout << "Test: role inferred from message types... ";
    const uint32_t sync     = PTPPeerObservation::bit(0x00);
    const uint32_t announce = PTPPeerObservation::bit(0x0B);
    const uint32_t followUp = PTPPeerObservation::bit(0x08);
    const uint32_t delayReq = PTPPeerObservation::bit(0x01);
    const uint32_t pdelayReq = PTPPeerObservation::bit(0x02);

    CHECK(PTPPeerObservation::roleFromMask(sync) == R::Master, "Sync alone is a master");
    CHECK(PTPPeerObservation::roleFromMask(announce) == R::Master, "Announce alone is a master");
    CHECK(PTPPeerObservation::roleFromMask(sync | followUp | announce) == R::Master,
          "Sync+Follow_Up+Announce is a master");
    CHECK(PTPPeerObservation::roleFromMask(delayReq) == R::Slave, "Delay_Req alone is a slave");
    CHECK(PTPPeerObservation::roleFromMask(pdelayReq) == R::Slave, "Pdelay_Req alone is a slave");
    CHECK(PTPPeerObservation::roleFromMask(sync | delayReq) == R::Mixed,
          "both master and slave traffic is Mixed (boundary-clock-like)");
    CHECK(PTPPeerObservation::roleFromMask(0) == R::Unknown, "no messages is Unknown");
    // Delay_Resp (0x09) is neither a master nor a slave indicator on its own.
    CHECK(PTPPeerObservation::roleFromMask(PTPPeerObservation::bit(0x09)) == R::Unknown,
          "Delay_Resp alone doesn't classify");
    std::cout << "PASS" << std::endl;
    return true;
}

bool testOuiExtraction() {
    std::cout << "Test: OUI is the first three clock-identity bytes... ";
    PTPPeerObservation o;
    o.clockId = idWithOui(0x00, 0x1B, 0x21, 0x42);
    auto oui = o.oui();
    CHECK(oui[0] == 0x00 && oui[1] == 0x1B && oui[2] == 0x21, "OUI bytes wrong");
    CHECK(o.ouiString() == "00:1b:21", "OUI string wrong: " + o.ouiString());
    std::cout << "PASS" << std::endl;
    return true;
}

bool testDedupAndCount() {
    std::cout << "Test: same identity is one row; distinct identities counted by role... ";
    PTPPeerTable t;
    auto now = Clock::now();

    auto master = idWithOui(0xAA, 0xBB, 0xCC, 0x01);
    auto slave1 = idWithOui(0xDD, 0xEE, 0xFF, 0x01);
    auto slave2 = idWithOui(0xDD, 0xEE, 0xFF, 0x02); // same OUI, different unit

    // One master seen twice — still one row, count 2.
    t.record(master, 0x0B, "10.0.0.1", 109, now); // Announce
    t.record(master, 0x00, "10.0.0.1", 109, now); // Sync
    // Two distinct slaves (two chained units).
    t.record(slave1, 0x01, "10.0.0.2", 109, now); // Delay_Req
    t.record(slave2, 0x01, "10.0.0.3", 109, now); // Delay_Req

    CHECK(t.size() == 3, "three distinct identities");
    CHECK(t.countByRole(R::Master) == 1, "one master");
    CHECK(t.countByRole(R::Slave) == 2, "two slaves = two units");

    // The master row accumulated both message types and bumped its count.
    for (const auto& p : t.peers()) {
        if (p.clockId == master) {
            CHECK(p.messageCount == 2, "master seen twice");
            CHECK(p.role() == R::Master, "master role");
        }
    }
    std::cout << "PASS" << std::endl;
    return true;
}

bool testTimeoutEviction() {
    std::cout << "Test: a peer that stops appearing ages out... ";
    PTPPeerTable t;
    auto t0 = Clock::now();
    auto a = idWithOui(0x11, 0x22, 0x33, 0x01);
    auto b = idWithOui(0x44, 0x55, 0x66, 0x01);

    t.record(a, 0x0B, "10.0.0.1", 0, t0);
    // b keeps appearing later; a does not.
    auto later = t0 + PTPPeerTable::kPeerTimeout + std::chrono::seconds(1);
    t.record(b, 0x0B, "10.0.0.2", 0, later);

    t.sweep(later);
    CHECK(t.size() == 1, "the stale peer should be gone");
    for (const auto& p : t.peers()) {
        CHECK(p.clockId == b, "only the still-present peer remains");
    }
    std::cout << "PASS" << std::endl;
    return true;
}

int main() {
    std::cout << std::endl << "PTP Peer Table Tests" << std::endl << std::endl;
    testRoleInference();
    testOuiExtraction();
    testDedupAndCount();
    testTimeoutEviction();
    std::cout << std::endl << "Passed: " << passed << ", Failed: " << failed << std::endl;
    return failed == 0 ? 0 : 1;
}
