//
// TestOptions.cpp
// AES67 Linux PTP daemon
//
// The command line, which decides what this box announces itself to be.
//
// It was read with std::atoi inside main(), and main.cpp is Linux-only, so
// nothing had ever run it: on the machine this code is written on it did not
// even compile. What that hid is not a cosmetic bug. priority1 is the first
// field IEEE 1588's BMCA compares and lower wins, so a word where a number
// belonged became 0 -- the value that wins outright -- and `--priority1 300`
// truncated into 44 and won nearly as hard. Either way a typo in the systemd
// unit made this box the grandmaster of the whole segment without a word.
//
// Options.cpp exists so that this file can call it. It is the same split
// PtpWire.cpp already has: the half that is only numbers builds anywhere.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "Options.h"

#include <string>
#include <vector>

using namespace AES67::LinuxPtpd;

namespace {

/// parseCommandLine takes argv as the C runtime hands it over, so the words
/// have to be laid out the same way: writable, argv[0] first.
CommandLineResult parse(const std::vector<std::string>& words, CommandLine& out) {
    std::vector<std::string> owned;
    owned.reserve(words.size() + 1);
    owned.emplace_back("aes67-ptpd");
    for (const auto& word : words) owned.push_back(word);

    std::vector<char*> argv;
    argv.reserve(owned.size());
    for (auto& word : owned) argv.push_back(word.data());

    return parseCommandLine(static_cast<int>(argv.size()), argv.data(), out);
}

} // namespace

TEST_CASE("An Empty Command Line Is The Documented Default") {
    CommandLine cli;
    REQUIRE(parse({}, cli) == CommandLineResult::Ok);

    // 128 is IEEE 1588's own default for both priorities, and what the usage
    // text promises. A daemon started with no arguments must not be a more
    // eager grandmaster than one started with the defaults spelled out.
    CHECK(cli.config.priority1 == 128);
    CHECK(cli.config.priority2 == 128);
    CHECK(cli.config.currentUtcOffset == 37);
    CHECK(cli.config.profileName == "aes67");
    CHECK(cli.config.interfaceName == "eth0");
    CHECK_FALSE(cli.config.verbose);
    CHECK_FALSE(cli.allowSoftwareTimestamps);
    CHECK_FALSE(cli.useReference);
    CHECK(cli.referenceChannel == 0);
    CHECK(cli.phcDevice.empty());
}

TEST_CASE("A Priority That Is Not A Number Does Not Become Zero") {
    CommandLine cli;

    // This is the whole reason the parsing moved out of main(): atoi answered
    // 0, 0 wins the BMCA outright, and nothing said anything.
    CHECK(parse({"--priority1", "onetwentyeight"}, cli) == CommandLineResult::Bad);
    CHECK(cli.config.priority1 == 128);

    CHECK(parse({"--priority2", "high"}, cli) == CommandLineResult::Bad);
    CHECK(cli.config.priority2 == 128);
}

TEST_CASE("A Priority Outside One Octet Does Not Wrap Into One") {
    CommandLine cli;

    // 300 used to truncate to 44, which is a far better clock than the 128
    // that was meant, and wins against almost anything on the segment.
    CHECK(parse({"--priority1", "300"}, cli) == CommandLineResult::Bad);
    CHECK(cli.config.priority1 == 128);

    CHECK(parse({"--priority1", "-1"}, cli) == CommandLineResult::Bad);
    CHECK(cli.config.priority1 == 128);
}

TEST_CASE("The Two Ends Of The Octet Are Still Legal") {
    CommandLine cli;

    // 0 is a real answer -- "this clock wins, by configuration" -- and
    // refusing garbage must not have made it unreachable.
    REQUIRE(parse({"--priority1", "0", "--priority2", "255"}, cli) == CommandLineResult::Ok);
    CHECK(cli.config.priority1 == 0);
    CHECK(cli.config.priority2 == 255);
}

TEST_CASE("A Trailing Unit Is Not Silently Dropped") {
    CommandLine cli;

    // atoi and stoi both stop at the first character that is not a digit and
    // report nothing, so "37s" was 37 and "0x80" was 0.
    CHECK(parse({"--utc-offset", "37s"}, cli) == CommandLineResult::Bad);
    CHECK(parse({"--priority1", "0x80"}, cli) == CommandLineResult::Bad);
}

TEST_CASE("The UTC Offset Is A Signed Sixteen-Bit Field") {
    CommandLine cli;

    // Negative is legal: it is a signed field, and the sign is the point.
    REQUIRE(parse({"--utc-offset", "-4"}, cli) == CommandLineResult::Ok);
    CHECK(cli.config.currentUtcOffset == -4);

    CHECK(parse({"--utc-offset", "40000"}, cli) == CommandLineResult::Bad);
}

TEST_CASE("A Flag Given As The Last Word Says A Value Is Missing") {
    CommandLine cli;

    CHECK(parse({"--interface"}, cli) == CommandLineResult::Bad);
    CHECK(parse({"--priority1"}, cli) == CommandLineResult::Bad);
    CHECK(parse({"--phc"}, cli) == CommandLineResult::Bad);

    // And the option before it was still read, rather than the whole line
    // being thrown away.
    CommandLine partial;
    CHECK(parse({"--interface", "eth1", "--priority1"}, partial) == CommandLineResult::Bad);
    CHECK(partial.config.interfaceName == "eth1");
}

TEST_CASE("An Option Nobody Has Heard Of Is Refused") {
    CommandLine cli;
    CHECK(parse({"--priority"}, cli) == CommandLineResult::Bad);
    CHECK(parse({"--priorityone", "128"}, cli) == CommandLineResult::Bad);
}

TEST_CASE("Asking For The Usage Is Not An Error") {
    CommandLine cli;
    CHECK(parse({"--help"}, cli) == CommandLineResult::UsagePrinted);
    CHECK(parse({"-h"}, cli) == CommandLineResult::UsagePrinted);
}

TEST_CASE("Naming A Reference Channel Turns The Reference On") {
    CommandLine cli;

    // --reference-channel without --reference used to imply it, and still
    // has to: asking for a channel of something that is off is not an
    // instruction anybody means.
    REQUIRE(parse({"--reference-channel", "3"}, cli) == CommandLineResult::Ok);
    CHECK(cli.useReference);
    CHECK(cli.referenceChannel == 3);
}

TEST_CASE("Everything The Usage Text Lists Is Accepted Together") {
    CommandLine cli;

    REQUIRE(parse({"--interface", "eth1",
                   "--profile", "aes67-tight",
                   "--priority1", "10",
                   "--priority2", "20",
                   "--utc-offset", "37",
                   "--phc", "/dev/ptp0",
                   "--reference",
                   "--reference-channel", "1",
                   "--allow-software-timestamps",
                   "--verbose"}, cli) == CommandLineResult::Ok);

    CHECK(cli.config.interfaceName == "eth1");
    CHECK(cli.config.profileName == "aes67-tight");
    CHECK(cli.config.priority1 == 10);
    CHECK(cli.config.priority2 == 20);
    CHECK(cli.config.currentUtcOffset == 37);
    CHECK(cli.phcDevice == "/dev/ptp0");
    CHECK(cli.useReference);
    CHECK(cli.referenceChannel == 1);
    CHECK(cli.allowSoftwareTimestamps);
    CHECK(cli.config.verbose);
}
