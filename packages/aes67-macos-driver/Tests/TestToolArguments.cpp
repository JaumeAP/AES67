//
// TestToolArguments.cpp
// AES67 macOS Driver
//
// The command-line surface of the tools in Tools/, run as a user runs them.
//
// Their argument parsing is inside a main(), so there is nothing to link
// against and nothing to call: this suite spawns the built binaries and
// reads what they print and what they exit with. The paths come from CMake
// (Tools/CMakeLists.txt), which is where the targets are, so the suite never
// has to guess at a build layout.
//
// What it is here to stop coming back: every one of these tools read its
// numbers with atoi()/atof(), which answer 0 for anything that is not a
// number. `--duration abc` therefore ran AES67TestReceiver forever instead
// of ten seconds, and a flag given as the last word on the command line was
// reported as an unknown option rather than as a missing value. None of that
// is visible from a build, which is why it survived.
//
// Every invocation below exits on its own, immediately: usage, a refused
// argument, an unknown option. Nothing here starts a tool that would open a
// socket and wait.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "support/ToolProcess.h"

#include <string>
#include <vector>

using namespace AES67::TestSupport;

namespace {

/// One program, and what it exits with when it cannot read its command line.
/// The six tools under Tools/ answer 1; aes67ptpd answers 2, as it did before
/// any of this, and that is its contract with the LaunchDaemon that starts it.
struct Tool {
    std::string path;
    int badStatus{1};
};

/// Every program in this package that takes options. The two offline
/// simulations take none and are CTests of their own.
///
/// AES67LiveDaemonInterop is here although only a Linux job ever runs it for
/// real, and aes67ptpd although it needs root to do anything: both read their
/// command line before they open anything, so the refusals below are exactly
/// as reachable on this machine as the others'.
const std::vector<Tool>& tools() {
    static const std::vector<Tool> all = {
        {AES67_TOOL_SENDER, 1},
        {AES67_TOOL_RECEIVER, 1},
        {AES67_TOOL_SAP_MONITOR, 1},
        {AES67_TOOL_PTP_STRESS, 1},
        {AES67_TOOL_LIVE_INTEROP, 1},
        {AES67_TOOL_PTP_DAEMON, 2},
    };
    return all;
}

} // namespace

TEST_CASE("Every Tool Answers For Itself And Leaves") {
    for (const auto& one : tools()) {
        const std::string tool = one.path;
        INFO("tool: " << tool);
        const ToolRun result = runTool(tool, "--help");

        // Asking for the usage is not an error, and a tool that printed it
        // and then went on to open a socket would hang this suite.
        CHECK(result.status == 0);
        CHECK(mentions(result.output, "Usage:"));
    }
}

TEST_CASE("An Option Nobody Has Heard Of Is Refused By Name") {
    for (const auto& one : tools()) {
        const std::string tool = one.path;
        const int badStatus = one.badStatus;
        INFO("tool: " << tool);
        const ToolRun result = runTool(tool, "--there-is-no-such-option");

        CHECK(result.status == badStatus);
        CHECK(mentions(result.output, "Unknown option"));
        CHECK(mentions(result.output, "--there-is-no-such-option"));
    }
}

TEST_CASE("A Flag Given As The Last Word Says A Value Is Missing") {
    // The old parsers folded `i + 1 < argc` into the match, so a flag with
    // nothing after it fell through to the unknown-option branch and was
    // reported as an option that does not exist. It does exist; its value
    // does not.
    const std::vector<ToolCase> cases = {
        {AES67_TOOL_SENDER, "--port"},
        {AES67_TOOL_SENDER, "--ip"},
        {AES67_TOOL_RECEIVER, "--duration"},
        {AES67_TOOL_SAP_MONITOR, "--group"},
        {AES67_TOOL_PTP_STRESS, "--seconds"},
        {AES67_TOOL_PTP_STRESS, "--csv"},
        {AES67_TOOL_LIVE_INTEROP, "--host"},
        {AES67_TOOL_LIVE_INTEROP, "--http-port"},
        {AES67_TOOL_PTP_DAEMON, "--domain", 2},
        {AES67_TOOL_PTP_DAEMON, "--interface", 2},
    };

    for (const auto& one : cases) {
        const std::string tool = one.tool;
        const std::string flag = one.arguments;
        INFO("tool: " << tool << " flag: " << flag);
        const ToolRun result = runTool(tool, flag);

        CHECK(result.status == one.status);
        CHECK(mentions(result.output, "needs a value"));
        CHECK(mentions(result.output, flag));
        CHECK_FALSE(mentions(result.output, "Unknown option"));
    }
}

TEST_CASE("A Value That Is Not A Number Is Refused Rather Than Read As Zero") {
    const std::vector<ToolCase> cases = {
        {AES67_TOOL_SENDER, "--port abc"},
        {AES67_TOOL_SENDER, "--channels 8ch"},
        {AES67_TOOL_SENDER, "--rate ''"},
        {AES67_TOOL_RECEIVER, "--duration abc"},
        {AES67_TOOL_SAP_MONITOR, "--port 5004x"},
        {AES67_TOOL_PTP_STRESS, "--seconds 30s"},
        {AES67_TOOL_LIVE_INTEROP, "--http-port 8080x"},
        // stoi stopped at the first character that was not a digit and said
        // nothing, so this used to configure domain 0.
        {AES67_TOOL_PTP_DAEMON, "--domain 0x10", 2},
        {AES67_TOOL_PTP_DAEMON, "--delay-req-ms 1s", 2},
    };

    for (const auto& one : cases) {
        const std::string tool = one.tool;
        const std::string arguments = one.arguments;
        INFO("tool: " << tool << " arguments: " << arguments);
        const ToolRun result = runTool(tool, arguments);

        CHECK(result.status == one.status);
        CHECK(mentions(result.output, "whole number"));
    }
}

TEST_CASE("A Number Too Big For What Holds It Is Refused") {
    // 70000 does not fit a uint16_t and used to be truncated into a port
    // number nobody asked for; 300 channels is more than the 128 this driver
    // has.
    const std::vector<ToolCase> cases = {
        {AES67_TOOL_SENDER, "--port 70000"},
        {AES67_TOOL_SENDER, "--channels 300"},
        {AES67_TOOL_RECEIVER, "--port 0"},
        {AES67_TOOL_SAP_MONITOR, "--duration -5"},
        {AES67_TOOL_PTP_STRESS, "--event-port 99999"},
        {AES67_TOOL_LIVE_INTEROP, "--rtsp-port 70000"},
        {AES67_TOOL_LIVE_INTEROP, "--poll-interval-ms 0"},
        // domainNumber is one octet on the wire and DSCP is six bits.
        {AES67_TOOL_PTP_DAEMON, "--domain 300", 2},
        {AES67_TOOL_PTP_DAEMON, "--dscp 999", 2},
        {AES67_TOOL_PTP_DAEMON, "--delay-req-ms 0", 2},
    };

    for (const auto& one : cases) {
        const std::string tool = one.tool;
        const std::string arguments = one.arguments;
        INFO("tool: " << tool << " arguments: " << arguments);
        const ToolRun result = runTool(tool, arguments);

        CHECK(result.status == one.status);
        CHECK(mentions(result.output, "must be between"));
    }
}

TEST_CASE("A Frequency That Is Not A Number Is Refused") {
    // --freq is the one value read as a real number, so it has a refusal of
    // its own to check: atof() answered 0.0 for a word, and a sine generator
    // asked for 0 Hz emits a DC offset rather than a tone.
    const ToolRun word = runTool(AES67_TOOL_SENDER, "--freq kilohertz");
    CHECK(word.status == 1);
    CHECK(mentions(word.output, "wants a number"));

    const ToolRun negative = runTool(AES67_TOOL_SENDER, "--freq -1000");
    CHECK(negative.status == 1);
    CHECK(mentions(negative.output, "must be between"));
}

TEST_CASE("A Negative SSRC Does Not Wrap Around Into A Valid One") {
    // strtoul accepts a leading minus and wraps it, so `--ssrc -1` used to
    // become 0xFFFFFFFF -- a perfectly usable SSRC that nobody had asked
    // for, sent on the wire under that identity.
    const ToolRun result = runTool(AES67_TOOL_SENDER, "--ssrc -1");

    CHECK(result.status == 1);
    CHECK(mentions(result.output, "unsigned number"));
}

TEST_CASE("A Value In Range Is Still Taken") {
    // The complement of every refusal above: hardening the parsing must not
    // have made it refuse what it is for. Nothing here reaches a socket --
    // the flags are parsed in the order they are written and --help is last,
    // so everything before it was accepted before the usage was printed.
    const ToolRun sender = runTool(AES67_TOOL_SENDER, "--port 5004 --channels 8 --ssrc 0xDEADBEEF --freq 997.5 --help");
    CHECK(sender.status == 0);
    CHECK(mentions(sender.output, "Usage:"));

    const ToolRun stress = runTool(AES67_TOOL_PTP_STRESS, "--seconds 30 --event-port 20319 --help");
    CHECK(stress.status == 0);
    CHECK(mentions(stress.output, "Usage:"));

    // The whole command line the CI job hands the live interop tool, which is
    // the one that used to accept a typo in any of these without a word.
    const ToolRun live = runTool(AES67_TOOL_LIVE_INTEROP,
        "--host 127.0.0.1 --rtsp-port 8854 --http-port 8080 "
        "--sap-group 239.255.255.255 "
        "--daemon-source-id 0 --daemon-source-name 'CI Fake Source' "
        "--audio-group 239.1.0.77 --audio-port 5104 "
        "--sink-id 1 --audio-seconds 4 "
        "--poll-timeout-ms 20000 --poll-interval-ms 500 --help");
    CHECK(live.status == 0);
    CHECK(mentions(live.output, "Usage:"));
}
