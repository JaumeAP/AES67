//
// AES67PTPStressRun.cpp
// Standalone CLI tool: runs this driver's real PTPMaster and PTPSlave
// against each other over a live network interface for a sustained period,
// sampling lock state, offset and mean path delay once a second.
//
// TestPTPLoopback (Tests/TestPTPLoopback.cpp) proves the exchange completes
// -- Announce, Sync, Follow_Up, Delay_Req, Delay_Resp, lock -- in about 8
// seconds and checks only the state at the end. Nothing in this repository
// had ever run the exchange for minutes at a time, or watched it while the
// same interface carried real, unrelated AES67-shaped traffic (a job
// Eines' aoip-stress-lab, a separate tool, is for -- run its `load`
// alongside this one; neither knows the other exists). This is that: the
// same two classes, run longer, with every second's state printed instead
// of thrown away, so a lock dropped under load would show up in the CSV
// rather than being invisible between TestPTPLoopback's single start and
// end check.
//
// Usage:
//   ./AES67PTPStressRun [options]
//
// Options:
//   --interface <name>  Interface to run the exchange on (default: lo0)
//   --seconds <n>        Duration in seconds (default: 30)
//   --event-port <n>     PTP event port (default: 20319 -- not 319, so this
//                         can run alongside a real PTP stack on the same host)
//   --general-port <n>   PTP general port (default: 20320)
//
// Output: one CSV line per second to stdout (t,locked,offsetNs,pathDelayNs,
// syncSent,delayRespSent,announceSent), a summary to stderr at the end.
//
#include "Shared/ToolOptions.h"

#include "NetworkEngine/PTP/PTPMaster.h"
#include "NetworkEngine/PTP/PTPSlave.h"
#include "NetworkEngine/PTP/PTPClockSource.h"

#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

using namespace AES67;

namespace {

volatile sig_atomic_t g_running = 1;
void onSignal(int) { g_running = 0; }

void printUsage(const char* argv0) {
    std::printf("Usage: %s [options]\n\n", argv0);
    std::printf("Options:\n");
    std::printf("  --interface <name>  Interface to run on (default: lo0)\n");
    std::printf("  --seconds <n>       Duration in seconds (default: 30)\n");
    std::printf("  --event-port <n>    PTP event port (default: 20319)\n");
    std::printf("  --general-port <n>  PTP general port (default: 20320)\n");
    std::printf("  --csv <path>        Write the samples to this file instead of stdout\n");
}

} // namespace

int main(int argc, char** argv) {
    std::string interfaceName = "lo0";
    int durationSec = 30;
    uint16_t eventPort = 20319;
    uint16_t generalPort = 20320;
    std::string csvPath;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        long long number = 0;

        if (arg == "--interface") {
            const char* text = AES67::ToolOptions::value(argc, argv, i);
            if (text == nullptr) { printUsage(argv[0]); return 1; }
            interfaceName = text;
        }
        else if (arg == "--seconds") {
            if (!AES67::ToolOptions::integerOption(argc, argv, i, 1, 2147483647LL, number)) return 1;
            durationSec = static_cast<int>(number);
        }
        else if (arg == "--event-port") {
            if (!AES67::ToolOptions::integerOption(argc, argv, i, 1, 65535, number)) return 1;
            eventPort = static_cast<uint16_t>(number);
        }
        else if (arg == "--general-port") {
            if (!AES67::ToolOptions::integerOption(argc, argv, i, 1, 65535, number)) return 1;
            generalPort = static_cast<uint16_t>(number);
        }
        else if (arg == "--csv") {
            const char* text = AES67::ToolOptions::value(argc, argv, i);
            if (text == nullptr) { printUsage(argv[0]); return 1; }
            csvPath = text;
        }
        else if (arg == "--help" || arg == "-h") { printUsage(argv[0]); return 0; }
        else { AES67::ToolOptions::unknownOption(arg.c_str()); printUsage(argv[0]); return 1; }
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    InternalClockSource clock;

    PTPMasterConfig masterConfig;
    masterConfig.interfaceName = interfaceName;
    masterConfig.eventPort = eventPort;
    masterConfig.generalPort = generalPort;
    masterConfig.announceIntervalMs = 250;
    masterConfig.syncIntervalMs = 125;
    masterConfig.priority1 = 1; // win BMCA outright: this is the only master here

    PTPSlaveConfig slaveConfig;
    slaveConfig.interfaceName = interfaceName;
    slaveConfig.eventPort = eventPort;
    slaveConfig.generalPort = generalPort;
    slaveConfig.announceIntervalMs = 250;
    slaveConfig.delayReqIntervalMs = 250;
    // Both ends live on this host: without loopback the kernel never
    // delivers the slave's Delay_Req to the master's own socket.
    slaveConfig.multicastLoopback = true;

    PTPMaster master(masterConfig, clock);
    PTPSlave slave(slaveConfig);

    if (!master.start()) { std::fprintf(stderr, "master.start() failed\n"); return 1; }
    if (!slave.start()) { std::fprintf(stderr, "slave.start() failed\n"); return 1; }

    // PTPMaster and PTPSlave write their own progress to stdout, so a run
    // captured with `> run.csv` produced a file with "[PTPSlave] LOCKED to
    // master" in the middle of the samples -- not a CSV any reader takes. With
    // --csv the samples go to their own file and stdout stays the log.
    std::FILE* csv = stdout;
    if (!csvPath.empty()) {
        csv = std::fopen(csvPath.c_str(), "w");
        if (csv == nullptr) {
            std::fprintf(stderr, "could not open %s for writing\n", csvPath.c_str());
            slave.stop();
            master.stop();
            return 1;
        }
        std::printf("[stress] samples to %s\n", csvPath.c_str());
        std::fflush(stdout);
    }

    std::fprintf(csv, "t,locked,offsetNs,pathDelayNs,syncSent,delayRespSent,announceSent\n");
    std::fflush(csv);

    int unlockedSamples = 0;
    long long maxAbsOffset = 0;
    long long maxAbsDelay = 0;

    for (int t = 0; t < durationSec && g_running; ++t) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        bool locked = slave.isLocked();
        long long offset = static_cast<long long>(slave.getOffsetNs());
        long long delay = static_cast<long long>(slave.getMeanPathDelayNs());
        if (!locked) unlockedSamples++;
        if (std::llabs(offset) > maxAbsOffset) maxAbsOffset = std::llabs(offset);
        if (std::llabs(delay) > maxAbsDelay) maxAbsDelay = std::llabs(delay);
        std::fprintf(csv, "%d,%d,%lld,%lld,%d,%d,%d\n",
                     t, locked ? 1 : 0, offset, delay,
                     master.syncSentCount(), master.delayRespSentCount(), master.announceSentCount());
        std::fflush(csv);
    }

    std::fprintf(stderr,
                 "\nSUMMARY: seconds=%d unlockedSamples=%d maxAbsOffsetNs=%lld maxAbsPathDelayNs=%lld finalLocked=%d\n",
                 durationSec, unlockedSamples, maxAbsOffset, maxAbsDelay, slave.isLocked() ? 1 : 0);

    if (csv != stdout) std::fclose(csv);

    slave.stop();
    master.stop();
    return 0;
}
