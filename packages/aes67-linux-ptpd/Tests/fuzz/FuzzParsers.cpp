//
// FuzzParsers.cpp
// aes67-linux-ptpd
//
// The one function of this daemon that reads bytes off the wire, fed what nobody wrote a test for.
//
// Two entry points over one function. LLVMFuzzerTestOneInput is for
// `-fsanitize=fuzzer` (-DAES67_LIBFUZZER=ON), which CI has and Apple's clang
// does not ship. main is a deterministic driver over the same function --
// fixed seed, printed -- so the check runs everywhere and a failure is one
// command away from being reproduced.
//
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "PtpWire.h"

using namespace AES67;
using namespace AES67::LinuxPtpd;



namespace {

std::vector<std::vector<uint8_t>> seedCorpus() {
    // A 44-byte Sync header: type 0, version 2, length 44, domain 0.
    std::vector<uint8_t> sync(44, 0);
    sync[0] = 0x00; sync[1] = 0x02; sync[2] = 0x00; sync[3] = 44;
    // A 64-byte Announce: type 0x0b.
    std::vector<uint8_t> announce(64, 0);
    announce[0] = 0x0b; announce[1] = 0x02; announce[2] = 0x00; announce[3] = 64;
    return {sync, announce, std::vector<uint8_t>(34, 0xff)};
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > 4096) return 0;
    PTPHeader header{};
    (void)parseHeader(data, size, header);
    return 0;
}

#ifndef FUZZ_LIBFUZZER
int main(int argc, char** argv) {
    const unsigned long iterations = (argc > 1) ? strtoul(argv[1], nullptr, 10) : 200000;
    const unsigned seed = (argc > 2) ? (unsigned)strtoul(argv[2], nullptr, 10) : 20260910u;
    printf("fuzz: %lu iterations, seed %u\n", iterations, seed);

    std::mt19937 rng(seed);
    const auto seeds = seedCorpus();

    for (unsigned long i = 0; i < iterations; ++i) {
        std::vector<uint8_t> input;
        // Half the run mutates something real, half is bytes from nowhere.
        if ((i & 1) == 0 && !seeds.empty()) {
            input = seeds[rng() % seeds.size()];
            const unsigned edits = 1 + (rng() % 8);
            for (unsigned e = 0; e < edits && !input.empty(); ++e) {
                input[rng() % input.size()] = (uint8_t)(rng() & 0xff);
            }
            if ((rng() % 4) == 0) input.resize(rng() % (input.size() + 1));
        } else {
            input.resize(rng() % 128);
            for (auto& b : input) b = (uint8_t)(rng() & 0xff);
        }
        LLVMFuzzerTestOneInput(input.data(), input.size());
    }

    printf("fuzz: no crash, no sanitizer report\n");
    return 0;
}
#endif
