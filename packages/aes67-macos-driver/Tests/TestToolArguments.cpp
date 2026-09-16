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

#include <sys/wait.h>

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace {

struct ToolRun {
    int status{-1};
    std::string output;
};

/// Runs one tool and collects its exit status and everything it wrote, both
/// streams together -- these tools print their usage on stdout and their
/// refusals on stderr, and a test that read only one of them would miss half
/// the answer.
ToolRun run(const std::string& tool, const std::string& arguments) {
    const std::string command = "'" + tool + "' " + arguments + " 2>&1";

    ToolRun result;
    std::FILE* pipe = popen(command.c_str(), "r");
    REQUIRE(pipe != nullptr);

    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result.output += buffer;
    }

    const int closed = pclose(pipe);
    result.status = WIFEXITED(closed) ? WEXITSTATUS(closed) : -1;
    return result;
}

/// One tool and the command line to hand it. A struct rather than a pair
/// because doctest's INFO builds a lambda around what it is given, and a
/// structured binding cannot be captured by one before C++20.
struct ToolCase {
    std::string tool;
    std::string arguments;
};

bool mentions(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

/// The four tools that take options and talk to the network. The two offline
/// simulations take none and are CTests of their own.
const std::vector<std::string>& tools() {
    static const std::vector<std::string> paths = {
        AES67_TOOL_SENDER, AES67_TOOL_RECEIVER, AES67_TOOL_SAP_MONITOR, AES67_TOOL_PTP_STRESS};
    return paths;
}

} // namespace

TEST_CASE("Every Tool Answers For Itself And Leaves") {
    for (const auto& tool : tools()) {
        INFO("tool: " << tool);
        const ToolRun result = run(tool, "--help");

        // Asking for the usage is not an error, and a tool that printed it
        // and then went on to open a socket would hang this suite.
        CHECK(result.status == 0);
        CHECK(mentions(result.output, "Usage:"));
    }
}

TEST_CASE("An Option Nobody Has Heard Of Is Refused By Name") {
    for (const auto& tool : tools()) {
        INFO("tool: " << tool);
        const ToolRun result = run(tool, "--there-is-no-such-option");

        CHECK(result.status == 1);
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
    };

    for (const auto& one : cases) {
        const std::string tool = one.tool;
        const std::string flag = one.arguments;
        INFO("tool: " << tool << " flag: " << flag);
        const ToolRun result = run(tool, flag);

        CHECK(result.status == 1);
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
    };

    for (const auto& one : cases) {
        const std::string tool = one.tool;
        const std::string arguments = one.arguments;
        INFO("tool: " << tool << " arguments: " << arguments);
        const ToolRun result = run(tool, arguments);

        CHECK(result.status == 1);
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
    };

    for (const auto& one : cases) {
        const std::string tool = one.tool;
        const std::string arguments = one.arguments;
        INFO("tool: " << tool << " arguments: " << arguments);
        const ToolRun result = run(tool, arguments);

        CHECK(result.status == 1);
        CHECK(mentions(result.output, "must be between"));
    }
}

TEST_CASE("A Frequency That Is Not A Number Is Refused") {
    // --freq is the one value read as a real number, so it has a refusal of
    // its own to check: atof() answered 0.0 for a word, and a sine generator
    // asked for 0 Hz emits a DC offset rather than a tone.
    const ToolRun word = run(AES67_TOOL_SENDER, "--freq kilohertz");
    CHECK(word.status == 1);
    CHECK(mentions(word.output, "wants a number"));

    const ToolRun negative = run(AES67_TOOL_SENDER, "--freq -1000");
    CHECK(negative.status == 1);
    CHECK(mentions(negative.output, "must be between"));
}

TEST_CASE("A Negative SSRC Does Not Wrap Around Into A Valid One") {
    // strtoul accepts a leading minus and wraps it, so `--ssrc -1` used to
    // become 0xFFFFFFFF -- a perfectly usable SSRC that nobody had asked
    // for, sent on the wire under that identity.
    const ToolRun result = run(AES67_TOOL_SENDER, "--ssrc -1");

    CHECK(result.status == 1);
    CHECK(mentions(result.output, "unsigned number"));
}

TEST_CASE("A Value In Range Is Still Taken") {
    // The complement of every refusal above: hardening the parsing must not
    // have made it refuse what it is for. Nothing here reaches a socket --
    // the flags are parsed in the order they are written and --help is last,
    // so everything before it was accepted before the usage was printed.
    const ToolRun sender = run(AES67_TOOL_SENDER, "--port 5004 --channels 8 --ssrc 0xDEADBEEF --freq 997.5 --help");
    CHECK(sender.status == 0);
    CHECK(mentions(sender.output, "Usage:"));

    const ToolRun stress = run(AES67_TOOL_PTP_STRESS, "--seconds 30 --event-port 20319 --help");
    CHECK(stress.status == 0);
    CHECK(mentions(stress.output, "Usage:"));
}
