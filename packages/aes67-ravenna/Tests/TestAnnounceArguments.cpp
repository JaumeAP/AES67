//
// TestAnnounceArguments.cpp
// AES67 RAVENNA
//
// ravenna-announce's command line, run as a user runs it.
//
// Its parsing is inside a main(), so the suite starts the binary and reads
// what it exits with -- support/ToolProcess.h, in the core, is that job, and
// the driver's own Tools/ suite does the same with the same header.
//
// What it is here to stop coming back: the numbers were read carefully, with
// strtol over the whole string, and then cast without anybody looking at the
// range. `--port 70000` does not fit a uint16_t, so it announced port 4464,
// and `--channels 70000` announced 4464 channels -- a session description
// that is wrong in a way no receiver can tell from one that was meant.
//
// Every invocation below either fails to parse or ends at --help. Nothing
// here reaches the point where the tool starts advertising.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "support/ToolProcess.h"

#include <string>
#include <vector>

using namespace AES67::TestSupport;

namespace {

/// A command line that parses in full, so that the refusals below are read
/// against something that is not itself refused. --help is last: the options
/// before it are parsed and accepted, and then the tool prints its usage and
/// leaves instead of going on to advertise anything.
const char* kRequired = "--interface lo0 --address 127.0.0.1";

} // namespace

TEST_CASE("A Command Line It Can Read Ends At The Usage") {
    const ToolRun result = runTool(AES67_TOOL_ANNOUNCE,
        std::string(kRequired) +
        " --port 5004 --rtsp-port 8554 --channels 2 --device-channel 0"
        " --rate 48000 --encoding L24 --ptime-us 1000"
        " --ptp-domain 0 --nmos-port 8080 --help");

    CHECK(result.status == 0);
    CHECK(mentions(result.output, "usage:"));
}

TEST_CASE("A Number Too Big For The Field It Goes In Is Refused") {
    const std::vector<ToolCase> cases = {
        // 70000 used to become 4464 in every one of these.
        {AES67_TOOL_ANNOUNCE, std::string(kRequired) + " --port 70000", 2},
        {AES67_TOOL_ANNOUNCE, std::string(kRequired) + " --rtsp-port 70000", 2},
        {AES67_TOOL_ANNOUNCE, std::string(kRequired) + " --nmos-port 70000", 2},
        {AES67_TOOL_ANNOUNCE, std::string(kRequired) + " --channels 70000", 2},
        // domainNumber is one octet on the wire.
        {AES67_TOOL_ANNOUNCE, std::string(kRequired) + " --ptp-domain 300", 2},
        // A port of 0 is not a port, and it is what a dropped digit gives.
        {AES67_TOOL_ANNOUNCE, std::string(kRequired) + " --port 0", 2},
    };

    for (const auto& one : cases) {
        const std::string arguments = one.arguments;
        INFO("arguments: " << arguments);
        const ToolRun result = runTool(one.tool, arguments);

        CHECK(result.status == one.status);
        CHECK(mentions(result.output, "must be between"));
    }
}

TEST_CASE("A Value That Is Not A Number Is Still Refused") {
    // This half already worked -- strtol over the whole string -- and has to
    // keep working now that the range check sits behind it.
    const ToolRun result = runTool(AES67_TOOL_ANNOUNCE,
                                   std::string(kRequired) + " --channels 2ch");

    CHECK(result.status == 2);
    CHECK(mentions(result.output, "whole number"));
}

TEST_CASE("A Flag Given As The Last Word Says A Value Is Missing") {
    const std::vector<ToolCase> cases = {
        {AES67_TOOL_ANNOUNCE, "--port", 2},
        {AES67_TOOL_ANNOUNCE, "--interface", 2},
        {AES67_TOOL_ANNOUNCE, "--encoding", 2},
    };

    for (const auto& one : cases) {
        const std::string arguments = one.arguments;
        INFO("arguments: " << arguments);
        const ToolRun result = runTool(one.tool, arguments);

        CHECK(result.status == one.status);
        CHECK(mentions(result.output, "needs a value"));
    }
}

TEST_CASE("An Option Nobody Has Heard Of Is Refused By Name") {
    const ToolRun result = runTool(AES67_TOOL_ANNOUNCE, "--there-is-no-such-option");

    CHECK(result.status == 2);
    CHECK(mentions(result.output, "Unknown option"));
    CHECK(mentions(result.output, "--there-is-no-such-option"));
}


TEST_CASE("Port zero still asks the kernel for one") {
    // RtspServer::start and HttpServer::start take 0 to mean "any free port"
    // and report back which one they got, and line 333 hands that to
    // mdns.start() for the SRV record. Bounding these two options to 1..65535
    // took the only way of asking for it away, and made openListenSocket's
    // own `if (port == 0)` branch unreachable from anything that ships.
    const ToolRun ephemeral = runTool(AES67_TOOL_ANNOUNCE,
        std::string(kRequired) + " --rtsp-port 0 --nmos-port 0 --help");

    CHECK(ephemeral.status == 0);
    CHECK(mentions(ephemeral.output, "usage:"));

    // A port that does not fit the field is still refused.
    const ToolRun tooBig = runTool(AES67_TOOL_ANNOUNCE,
                                   std::string(kRequired) + " --rtsp-port 65536");
    CHECK(tooBig.status == 2);
    CHECK(mentions(tooBig.output, "must be between"));
}
