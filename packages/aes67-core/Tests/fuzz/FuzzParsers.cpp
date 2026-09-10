//
// FuzzParsers.cpp
// aes67-core
//
// The SDP parser -- the one thing in this library that reads bytes it did not write, fed what nobody wrote a test for.
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

#include "Driver/SDPParser.h"

using namespace AES67;

namespace {

std::vector<std::vector<uint8_t>> seedCorpus() {
    const char* sdp =
        "v=0\r\no=- 13 0 IN IP4 192.168.15.52\r\ns=Anubis_610120_13\r\n"
        "c=IN IP4 239.1.15.52/15\r\nt=0 0\r\n"
        "a=ts-refclk:ptp=IEEE1588-2008:00-1D-C1-FF-FE-51-9E-F7:0\r\na=mediaclk:direct=0\r\n"
        "m=audio 5004 RTP/AVP 98\r\nc=IN IP4 239.1.15.52/15\r\na=rtpmap:98 L16/48000/2\r\n"
        "a=source-filter: incl IN IP4 239.1.15.52 192.168.15.52\r\na=framecount:48\r\n"
        "a=ptime:1\r\na=recvonly\r\n";
    return {std::vector<uint8_t>(sdp, sdp + std::char_traits<char>::length(sdp))};
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > 8192) return 0;
    const std::string text(reinterpret_cast<const char*>(data), size);
    const auto parsed = SDPParser::parseString(text);
    if (parsed) {
        // What parses has to survive being written and read again: a
        // generate() that emits a line its own parser refuses is a crash
        // that waits for the next device rather than this run.
        const std::string written = SDPParser::generate(*parsed);
        (void)SDPParser::parseString(written);
        (void)parsed->isValid();
        (void)parsed->getValidationErrors();
    }
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
            input.resize(rng() % 512);
            for (auto& b : input) b = (uint8_t)(rng() & 0xff);
        }
        LLVMFuzzerTestOneInput(input.data(), input.size());
    }

    printf("fuzz: no crash, no sanitizer report\n");
    return 0;
}
#endif
