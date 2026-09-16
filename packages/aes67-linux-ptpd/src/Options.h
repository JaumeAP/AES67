//
// Options.h
// AES67 Linux PTP daemon
// The command line, read and checked.
//
// It used to be read with std::atoi inside main(), which answers 0 to a word
// and to "0" alike, and the answer was then cast straight into a uint8_t. On
// a grandmaster that is not a cosmetic bug: priority1 is the first field of
// the BMCA comparison and 0 is the value that wins it outright, so a typo in
// the systemd unit -- `--priority1 onetwentyeight`, `--priority1 12 8` --
// made this box the grandmaster of the whole segment and said nothing.
// `--priority1 300` truncated to 44 and won it almost as hard.
//
// Read here rather than in main() so that a test can call it: main.cpp is
// Linux-only, this is not.
//
#pragma once

#include "GrandmasterConfig.h"

#include <string>

namespace AES67::LinuxPtpd {

/// Everything the command line settles, including what is not part of the
/// grandmaster's own configuration.
struct CommandLine {
    GrandmasterConfig config;
    std::string phcDevice;
    bool allowSoftwareTimestamps = false;
    bool useReference = false;
    unsigned int referenceChannel = 0;
};

enum class CommandLineResult {
    Ok,            // `out` is filled in; run
    UsagePrinted,  // --help; there is nothing to do and nothing has failed
    Bad,           // unreadable; the reason is already printed
};

/// What the daemon accepts, on stderr.
void usage();

/// Reads argv into `out`, leaving every field it was not given at its
/// default. Anything it refuses is reported before it returns, naming the
/// option rather than the function that could not read it.
CommandLineResult parseCommandLine(int argc, char** argv, CommandLine& out);

}  // namespace AES67::LinuxPtpd
