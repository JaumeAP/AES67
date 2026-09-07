//
// main.cpp
// AES67 Linux PTP daemon
// A PTP grandmaster for a Raspberry Pi 5, or any Linux machine whose NIC
// keeps a PTP hardware clock.
//
// Usage:
//   aes67-ptpd [--interface eth0] [--profile aes67] [--priority1 128]
//              [--priority2 128] [--utc-offset 37] [--phc /dev/ptp0]
//              [--reference] [--reference-channel 0]
//              [--allow-software-timestamps] [--verbose]
//
// It needs CAP_NET_ADMIN to turn the NIC's timestamping on and CAP_NET_BIND_SERVICE
// for ports 319 and 320. The systemd unit in systemd/ grants exactly those two
// and nothing else.
//
#include "ExternalReference.h"
#include "Grandmaster.h"
#include "PhcClock.h"
#include "PtpSockets.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

std::atomic<bool> g_running{true};

void handleSignal(int) { g_running.store(false, std::memory_order_release); }

void usage() {
    std::fprintf(stderr,
                 "usage: aes67-ptpd [--interface NAME] [--profile NAME]\n"
                 "                  [--priority1 N] [--priority2 N] [--utc-offset N]\n"
                 "                  [--phc /dev/ptpN] [--reference]\n"
                 "                  [--reference-channel N]\n"
                 "                  [--allow-software-timestamps] [--verbose]\n"
                 "\n"
                 "profiles: aes67, aes67-tight, default1588, gptp\n"
                 "          (packages/aes67-profiles holds the numbers)\n");
}

/// The argument after `name`, or nullptr when it is missing. Reporting the
/// missing value is the caller's, which is what lets it name the option.
const char* valueFor(int argc, char** argv, int& index) {
    if (index + 1 >= argc) return nullptr;
    return argv[++index];
}

}  // namespace

int main(int argc, char** argv) {
    using namespace AES67::LinuxPtpd;

    GrandmasterConfig config;
    std::string phcDevice;
    bool allowSoftwareTimestamps = false;
    bool useReference = false;
    unsigned int referenceChannel = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i];
        const char* value = nullptr;

        if (option == "--help" || option == "-h") {
            usage();
            return 0;
        } else if (option == "--allow-software-timestamps") {
            allowSoftwareTimestamps = true;
        } else if (option == "--verbose" || option == "-v") {
            config.verbose = true;
        } else if (option == "--reference") {
            useReference = true;
        } else if (option == "--reference-channel") {
            if ((value = valueFor(argc, argv, i)) == nullptr) { usage(); return 2; }
            useReference = true;
            referenceChannel = static_cast<unsigned int>(std::atoi(value));
        } else if (option == "--interface") {
            if ((value = valueFor(argc, argv, i)) == nullptr) { usage(); return 2; }
            config.interfaceName = value;
        } else if (option == "--profile") {
            if ((value = valueFor(argc, argv, i)) == nullptr) { usage(); return 2; }
            config.profileName = value;
        } else if (option == "--priority1") {
            if ((value = valueFor(argc, argv, i)) == nullptr) { usage(); return 2; }
            config.priority1 = static_cast<uint8_t>(std::atoi(value));
        } else if (option == "--priority2") {
            if ((value = valueFor(argc, argv, i)) == nullptr) { usage(); return 2; }
            config.priority2 = static_cast<uint8_t>(std::atoi(value));
        } else if (option == "--utc-offset") {
            if ((value = valueFor(argc, argv, i)) == nullptr) { usage(); return 2; }
            config.currentUtcOffset = static_cast<int16_t>(std::atoi(value));
        } else if (option == "--phc") {
            if ((value = valueFor(argc, argv, i)) == nullptr) { usage(); return 2; }
            phcDevice = value;
        } else {
            std::fprintf(stderr, "unknown option: %s\n", option.c_str());
            usage();
            return 2;
        }
    }

    // The status line is one a second and has to arrive as it is written: on
    // a pipe -- which is what systemd hands a service -- stdout is block
    // buffered by default, and a daemon that says nothing for a minute and
    // then says sixty things is not reporting, it is confessing.
    ::setvbuf(stdout, nullptr, _IOLBF, 0);

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    // A slave that goes away mid-send must not take the daemon with it.
    std::signal(SIGPIPE, SIG_IGN);

    PhcClock clock;
    std::string error;
    if (!clock.open(config.interfaceName, phcDevice, error)) {
        std::fprintf(stderr, "[ptpd] %s\n", error.c_str());
        if (!allowSoftwareTimestamps) {
            std::fprintf(stderr,
                         "[ptpd] refusing to announce a time this machine cannot "
                         "stamp; pass --allow-software-timestamps to run anyway\n");
            return 1;
        }
        std::fprintf(stderr, "[ptpd] carrying on with the system clock\n");
    }

    PtpSockets sockets;
    if (!sockets.open(config.interfaceName, allowSoftwareTimestamps, error)) {
        std::fprintf(stderr, "[ptpd] %s\n", error.c_str());
        return 1;
    }

    // The reference is what turns a precise clock into a right one. Without
    // it every device on the network agrees with this one and the whole
    // network drifts together, away from the studio.
    ExternalReference reference;
    ExternalReference* referencePointer = nullptr;
    if (useReference) {
        if (!reference.enable(clock, referenceChannel, error)) {
            std::fprintf(stderr, "[ptpd] %s\n", error.c_str());
            std::fprintf(stderr,
                         "[ptpd] refusing to run disciplined when the reference "
                         "cannot be stamped by the clock that stamps the packets\n");
            return 1;
        }
        referencePointer = &reference;
        std::printf("[ptpd] reference on channel %u of %s, free-running until it locks\n",
                    referenceChannel, clock.devicePath().c_str());
    }

    Grandmaster grandmaster(sockets, clock, config, referencePointer);
    if (!grandmaster.start(error)) {
        std::fprintf(stderr, "[ptpd] %s\n", error.c_str());
        return 1;
    }

    grandmaster.run(g_running);
    std::printf("[ptpd] stopped\n");
    return 0;
}
