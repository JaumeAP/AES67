// The parser, fed whatever arrives.
//
// parsePTPMessage() is the only thing in this library that reads bytes it did
// not write, and they come off a multicast group any device on the segment can
// send to. Its length guards are the part of the code a unit test can only
// check at the lengths somebody thought of; this feeds it the ones nobody did.
//
// Two entry points over one function:
//
//   LLVMFuzzerTestOneInput  for `clang++ -fsanitize=fuzzer,address,undefined`,
//                           which is what CI runs. Apple's clang ships no
//                           fuzzer runtime, so it cannot be built on the Mac
//                           this is written on.
//   main                    a deterministic driver over the same function, so
//                           the check runs everywhere. Same seed, same inputs,
//                           every run: a failure here is reproducible without
//                           a corpus file.
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "ptp/ptp-base.h"
#include "ptp_messages.h"

namespace {

// The library with its transports cut off: nothing is sent, no socket is
// opened, and the clock is never stepped. What is left is the parser and the
// state it keeps.
class FuzzPTP : public PTPBase
{
public:
    FuzzPTP(bool master_, bool slave_, bool p2p_) : PTPBase(master_, slave_, p2p_) {}
    using PTPBase::parsePTPMessage;

private:
    void initSockets() override {}
    void closeSockets() override {}
    void updateSockets() override {}
    void sendPTPMessage(const uint8_t *, int, bool, bool) override {}
};

} // namespace

// One input, through every port configuration the library has: master only,
// slave only, both, and peer to peer. A message refused by one is taken by
// another -- the type checks in parsePTPMessage() are written against these
// four -- so fuzzing one configuration leaves three parsers unvisited.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 4096) {
        return 0;
    }

    static const struct { bool master, slave, p2p; } roles[] = {
        {true, false, false}, {false, true, false},
        {true, true, false},  {true, true, true},
    };

    for (const auto &role : roles) {
        FuzzPTP ptp(role.master, role.slave, role.p2p);
        timespec ts;
        // A receive time that is not zero, so arithmetic against it is the
        // arithmetic the real path does.
        nanoTimeToTimespec((NanoTime)1234567890 * NS_PER_S + 42, ts);
        ptp.parsePTPMessage(data, (int)size, ts);
        // Twice: the second message meets the state the first one left, which
        // is where a parser that trusts its own bookkeeping goes wrong.
        ptp.parsePTPMessage(data, (int)size, ts);
    }
    return 0;
}

#ifndef FUZZ_LIBFUZZER
namespace {

// Inputs worth starting from: a valid message of each type this parser
// accepts, so a mutation lands inside a message the code will parse rather
// than being rejected at the first byte.
std::vector<std::vector<uint8_t>> seedCorpus()
{
    std::vector<std::vector<uint8_t>> seeds;
    // Ten bytes, not eight: makeAnnounce() wants the eight-byte clock
    // identity, and makeDelayRequest() wants a whole sourcePortIdentity --
    // that identity plus the two-byte port number.
    static const uint8_t identity[10] = {0x02, 0x11, 0x22, 0xff, 0xfe,
                                         0x33, 0x44, 0x55, 0x00, 0x01};
    seeds.push_back(makeAnnounce(1, identity, 128));
    seeds.push_back(makeSync(1));
    seeds.push_back(makeOneStepSync(1, 1000));
    seeds.push_back(makeFollowUp(1, 1000));
    seeds.push_back(makeResponse(9, 1, 2000));
    seeds.push_back(makeResponse(3, 1, 2000));
    seeds.push_back(makePdelayRespFollowUp(1, 3000));
    seeds.push_back(makeDelayRequest(1, identity));
    seeds.push_back(makePeerDelayRequest(1, identity));
    seeds.push_back(std::vector<uint8_t>(34, 0));
    seeds.push_back(std::vector<uint8_t>(64, 0xff));
    return seeds;
}

} // namespace

int main(int argc, char **argv)
{
    const unsigned long iterations = (argc > 1) ? strtoul(argv[1], nullptr, 10) : 200000;
    // Fixed, and printed, so a failure is one command away from being
    // reproduced rather than a bug that happened once on somebody's laptop.
    const unsigned seed = (argc > 2) ? (unsigned)strtoul(argv[2], nullptr, 10) : 20260910u;
    printf("fuzz: %lu iterations, seed %u\n", iterations, seed);

    std::mt19937 rng(seed);
    const auto seeds = seedCorpus();

    for (unsigned long i = 0; i < iterations; ++i) {
        std::vector<uint8_t> input;
        // Half the run mutates a real message, half is bytes from nowhere.
        // The first finds what a nearly valid message does; the second finds
        // what happens before the guards.
        if ((i & 1) == 0) {
            input = seeds[rng() % seeds.size()];
            const unsigned edits = 1 + (rng() % 8);
            for (unsigned e = 0; e < edits && !input.empty(); ++e) {
                input[rng() % input.size()] = (uint8_t)(rng() & 0xff);
            }
            // And sometimes truncated, which is what the length guards are for.
            if ((rng() % 4) == 0) {
                input.resize(rng() % (input.size() + 1));
            }
        } else {
            input.resize(rng() % 96);
            for (auto &b : input) {
                b = (uint8_t)(rng() & 0xff);
            }
        }
        LLVMFuzzerTestOneInput(input.data(), input.size());
    }

    printf("fuzz: no crash, no sanitizer report\n");
    return 0;
}
#endif
