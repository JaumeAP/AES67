//
// GrandmasterConfig.h
// AES67 Linux PTP daemon
// What the daemon was asked to be, as plain data.
//
// Split out of Grandmaster.h, which reaches PhcClock.h and PtpSockets.h and
// so only compiles on Linux. The settings themselves are numbers and strings
// and nothing else, and keeping them here is what lets the command line that
// fills them in (Options.h) be read and tested on the machine the code is
// written on -- the same split PtpWire.cpp already has, for the same reason.
//
#pragma once

#include <cstdint>
#include <string>

namespace AES67::LinuxPtpd {

struct GrandmasterConfig {
    std::string interfaceName = "eth0";
    /// The name of a profile in packages/aes67-profiles: "aes67",
    /// "aes67-tight", "default1588" or "gptp". The five numbers it fixes are
    /// not repeated here.
    std::string profileName = "aes67";
    uint8_t priority1 = 128;
    uint8_t priority2 = 128;
    int16_t currentUtcOffset = 37;
    bool verbose = false;
};

}  // namespace AES67::LinuxPtpd
