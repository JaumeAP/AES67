//
// FuzzDriver.h
// AES67 profiles
//
// The deterministic half of every fuzz harness in this repository.
//
// Each package fuzzes what it parses, and the parsing is all that differs:
// the driver around it -- read an iteration count, seed a generator, mutate a
// corpus entry or make bytes from nowhere, hand them to the entry point --
// was written five times and was the same five times.
//
// It lives here because this is the package everything else may include and
// which includes nothing itself. It carries no profile data and no platform:
// a header of pure standard library, the same as what it sits beside.
//
// A harness using it defines LLVMFuzzerTestOneInput and a seedCorpus(), then
// writes one line:
//
//     #ifndef FUZZ_LIBFUZZER
//     int main(int argc, char** argv) {
//         return AES67::Testing::runFuzzDriver(argc, argv, seedCorpus(), 256);
//     }
//     #endif
//
// libFuzzer, where a toolchain has it, uses LLVMFuzzerTestOneInput directly
// and never sees this file.
//
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

namespace AES67::Testing {

/// Runs `LLVMFuzzerTestOneInput` over mutated corpus entries and random bytes.
///
/// `argv[1]` is the iteration count and `argv[2]` the seed, both optional. The
/// seed is fixed by default and printed either way: a failure has to be one
/// command away from being reproduced, on this machine or another.
///
/// `maxRandomLength` bounds the from-nowhere half. It is per-harness because
/// what "too long to be interesting" means is: 128 bytes for a PTP header,
/// 1500 for an RTP frame.
inline int runFuzzDriver(int argc, char** argv,
                         const std::vector<std::vector<uint8_t>>& seeds,
                         unsigned maxRandomLength) {
    const unsigned long iterations =
        (argc > 1) ? strtoul(argv[1], nullptr, 10) : 200000;
    const unsigned seed =
        (argc > 2) ? static_cast<unsigned>(strtoul(argv[2], nullptr, 10)) : 20260910u;
    printf("fuzz: %lu iterations, seed %u\n", iterations, seed);

    std::mt19937 rng(seed);

    for (unsigned long i = 0; i < iterations; ++i) {
        std::vector<uint8_t> input;
        // Half the run mutates something real, half is bytes from nowhere.
        // The first finds what a nearly valid message does; the second finds
        // what happens before the guards.
        if ((i & 1) == 0 && !seeds.empty()) {
            input = seeds[rng() % seeds.size()];
            const unsigned edits = 1 + (rng() % 8);
            for (unsigned e = 0; e < edits && !input.empty(); ++e) {
                input[rng() % input.size()] = static_cast<uint8_t>(rng() & 0xff);
            }
            // And sometimes truncated, which is what length guards are for.
            if ((rng() % 4) == 0) {
                input.resize(rng() % (input.size() + 1));
            }
        } else {
            input.resize(rng() % maxRandomLength);
            std::generate(input.begin(), input.end(),
                          [&rng] { return static_cast<uint8_t>(rng() & 0xff); });
        }
        LLVMFuzzerTestOneInput(input.data(), input.size());
    }

    printf("fuzz: no crash, no sanitizer report\n");
    return 0;
}

} // namespace AES67::Testing
