//
// aes67ptpd.cpp
// AES67 macOS Driver
//
// The host's PTP engine, as one process. It runs a PTPSlave and publishes
// offset, path delay and lock state on a Unix-domain socket
// (Shared/PTPServiceProtocol.h); it holds no audio state and knows nothing
// about the driver.
//
// Not a privilege workaround: on macOS 26.6.2 an unprivileged process binds
// UDP 319 and 320 fine (measured). What this buys is one engine per host
// rather than one per process -- alive across coreaudiod restarts and plugin
// reloads, readable by the plugin and the Manager app at the same time, and
// exercisable on its own. Running it as a LaunchDaemon is about lifecycle,
// not about root.
//
// Usage:
//   aes67ptpd [--interface en0] [--domain 0] [--socket /var/run/aes67ptpd.sock]
//             [--mechanism e2e|p2p] [--delay-req-ms 1000] [--verbose]
//

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "NetworkEngine/PTP/PTPService.h"
#include "NetworkEngine/PTP/PTPSlave.h"

namespace {

std::atomic<bool> g_running{true};

void HandleSignal(int) { g_running.store(false); }

void Usage() {
    std::printf(
        "Usage: aes67ptpd [options]\n"
        "  --interface <name>   network interface (default en0)\n"
        "  --domain <n>         PTP domain (default 0, per AES67)\n"
        "  --socket <path>      status socket (default %s)\n"
        "  --mechanism e2e|p2p  delay mechanism (default e2e)\n"
        "  --delay-req-ms <n>   initial Delay_Req interval (default 1000)\n"
        "  --multicast-loopback receive our own multicast (same-host testing)\n"
        "  --verbose            log every published status\n",
        AES67::kPTPServiceSocketPath);
}

}  // namespace

int main(int argc, char** argv) {
    AES67::PTPSlaveConfig config;
    std::string socketPath = AES67::kPTPServiceSocketPath;
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", flag.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (flag == "--interface") {
            config.interfaceName = next();
        } else if (flag == "--domain") {
            config.domain = std::stoi(next());
        } else if (flag == "--socket") {
            socketPath = next();
        } else if (flag == "--mechanism") {
            const std::string mechanism = next();
            if (mechanism == "p2p") {
                config.delayMechanism = AES67::DelayMechanism::PeerToPeer;
            } else if (mechanism != "e2e") {
                std::fprintf(stderr, "--mechanism takes e2e or p2p\n");
                return 2;
            }
        } else if (flag == "--delay-req-ms") {
            config.delayReqIntervalMs = std::stoi(next());
        } else if (flag == "--multicast-loopback") {
            // Same-host testing: without it the kernel never delivers this
            // slave's Delay_Req to a master in another process on this
            // machine. Off in production, where it is pure noise.
            config.multicastLoopback = true;
        } else if (flag == "--verbose") {
            verbose = true;
        } else if (flag == "--help" || flag == "-h") {
            Usage();
            return 0;
        } else {
            std::fprintf(stderr, "unknown option: %s\n", flag.c_str());
            Usage();
            return 2;
        }
    }

    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);
    // A reader that disappears mid-write must not take the daemon with it.
    std::signal(SIGPIPE, SIG_IGN);

    AES67::PTPServiceServer server(socketPath);
    if (!server.start()) {
        std::cerr << "[aes67ptpd] could not create " << socketPath
                  << " -- another instance running, or not enough privilege"
                  << std::endl;
        return 1;
    }

    AES67::PTPSlave slave(config);
    if (!slave.start()) {
        // The usual cause is exactly the one this daemon exists for, so say
        // it plainly rather than leaving a bare failure.
        std::cerr << "[aes67ptpd] PTP slave failed to start on "
                  << config.interfaceName
                  << ". Check the interface name, and that nothing else already"
                     " holds UDP 319/320 on it." << std::endl;
        server.stop();
        return 1;
    }

    std::cout << "[aes67ptpd] publishing on " << socketPath << " (interface "
              << config.interfaceName << ", domain " << config.domain << ")"
              << std::endl;

    while (g_running.load()) {
        AES67::PTPServiceStatus status;
        status.locked = slave.isLocked() ? 1 : 0;
        status.clockClass = slave.getClockClass();
        status.clockAccuracy = slave.getClockAccuracy();
        status.domain = static_cast<uint8_t>(config.domain);
        status.offsetNs = slave.getOffsetNs();
        status.pathDelayNs = slave.getMeanPathDelayNs();
        status.frequencyDriftPpb = slave.getFrequencyDriftPpb();

        const std::string grandmaster = slave.getGrandmasterID();
        unsigned int octets[8] = {0};
        if (std::sscanf(grandmaster.c_str(),
                        "%x:%x:%x:%x:%x:%x:%x:%x", &octets[0], &octets[1],
                        &octets[2], &octets[3], &octets[4], &octets[5],
                        &octets[6], &octets[7]) == 8) {
            for (int i = 0; i < 8; ++i) {
                status.grandmasterIdentity[i] =
                    static_cast<uint8_t>(octets[i] & 0xFF);
            }
        }

        server.publish(status);
        if (verbose) {
            std::cout << "[aes67ptpd] locked=" << static_cast<int>(status.locked)
                      << " offset=" << status.offsetNs
                      << "ns delay=" << status.pathDelayNs
                      << "ns readers=" << server.clientCount() << std::endl;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(AES67::kPTPServiceHeartbeatMs));
    }

    std::cout << "[aes67ptpd] stopping" << std::endl;
    slave.stop();
    server.stop();
    return 0;
}
