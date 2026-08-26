//
// TestRTCPReceiverTable.cpp
// AES67 macOS Driver
// Pins the RTCP receiver-detection core: parsing SR/RR sender SSRCs and SDES
// CNAMEs from known bytes, malformed-packet robustness (no OOB), and the
// reporter aggregation + timeout. Header-only, no sockets.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/Discovery/RTCPReceiverTable.h"

#include <cstdint>
#include <iostream>
#include <random>
#include <vector>

using namespace AES67;
using Clock = std::chrono::steady_clock;

static void put32(std::vector<uint8_t>& v, uint32_t x){ v.push_back(x>>24); v.push_back(x>>16); v.push_back(x>>8); v.push_back(x); }

// A minimal RR: V=2,RC=0,PT=201,len=1 (2 words) + sender SSRC. len field = words-1 = 1.
static std::vector<uint8_t> makeRR(uint32_t ssrc){
    std::vector<uint8_t> v; v.push_back(0x80); v.push_back(kRTCP_RR); v.push_back(0); v.push_back(1);
    put32(v, ssrc); return v;
}

TEST_CASE("Parse RR") {
    std::cout << "Test: parse a Receiver Report's sender SSRC... ";
    auto p = makeRR(0xDEADBEEF);
    auto r = RTCPReceiverTable::parse(p.data(), p.size());
    CHECK(r.valid);
    CHECK((r.reporterSSRCs.size()==1 && r.reporterSSRCs[0]==0xDEADBEEF));
    std::cout << "PASS" << std::endl;
}

TEST_CASE("Parse Compound S Rplus SDES") {
    std::cout << "Test: compound SR + SDES yields SSRC and CNAME... ";
    // SR: V=2,PT=200,len=1,SSRC.
    std::vector<uint8_t> v; v.push_back(0x80); v.push_back(kRTCP_SR); v.push_back(0); v.push_back(1);
    put32(v, 0x11223344);
    // SDES: header V=2,PT=202,RC=1; then chunk SSRC + CNAME item + end.
    std::vector<uint8_t> sdes; sdes.push_back(0x81); sdes.push_back(kRTCP_SDES); // len filled later
    std::vector<uint8_t> chunk; put32(chunk, 0x11223344);
    const char* cn = "amp-1"; chunk.push_back(1); chunk.push_back((uint8_t)5);
    for(int i=0;i<5;i++) chunk.push_back(cn[i]);
    chunk.push_back(0); // end of items
    while(chunk.size()%4) chunk.push_back(0); // pad chunk
    // total SDES bytes = 4 header + chunk; length in words - 1
    size_t total = 4 + chunk.size(); uint16_t words = (uint16_t)(total/4 - 1);
    sdes.push_back(words>>8); sdes.push_back(words&0xFF);
    sdes.insert(sdes.end(), chunk.begin(), chunk.end());
    v.insert(v.end(), sdes.begin(), sdes.end());

    auto r = RTCPReceiverTable::parse(v.data(), v.size());
    CHECK(r.valid);
    CHECK((r.reporterSSRCs.size()==1 && r.reporterSSRCs[0]==0x11223344));
    bool found=false; for(auto&c:r.cnames) if(c.first==0x11223344 && c.second=="amp-1") found=true;
    CHECK(found);
    std::cout << "PASS" << std::endl;
}

TEST_CASE("Malformed No Crash") {
    std::cout << "Test: malformed/truncated RTCP never reads out of bounds... ";
    std::mt19937 rng(99);
    for(int i=0;i<200000;i++){
        size_t len = rng()%40;
        std::vector<uint8_t> v(len);
        for(auto&b:v) b=(uint8_t)rng();
        auto r = RTCPReceiverTable::parse(v.empty()?nullptr:v.data(), len); (void)r;
    }
    // A version!=2 packet is rejected.
    std::vector<uint8_t> bad = {0x00,201,0,1,0,0,0,1};
    CHECK(!RTCPReceiverTable::parse(bad.data(),bad.size()).valid);
    // Length field overrunning the buffer -> not fully valid, no crash.
    std::vector<uint8_t> over = {0x80,201,0,99, 1,2,3,4};
    auto ro = RTCPReceiverTable::parse(over.data(), over.size());
    CHECK(ro.reporterSSRCs.empty());
    std::cout << "PASS" << std::endl;
}

TEST_CASE("Aggregate And Timeout") {
    std::cout << "Test: distinct reporters counted, stale ones evicted... ";
    RTCPReceiverTable t; auto now=Clock::now();
    t.record(1,"10.0.0.1","",now);
    t.record(1,"10.0.0.1","",now); // same reporter twice -> one row
    t.record(2,"10.0.0.2","amp-2",now);
    t.record(3,"10.0.0.3","",now);
    CHECK(t.size()==3);
    // Let #1 go stale, refresh #2/#3.
    auto later = now + RTCPReceiverTable::kReporterTimeout + std::chrono::seconds(1);
    t.record(2,"10.0.0.2","",later); t.record(3,"10.0.0.3","",later);
    t.sweep(later);
    CHECK(t.size()==2);
    std::cout << "PASS" << std::endl;
}

