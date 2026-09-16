//
// ToolProcess.h
// AES67 core - test support
// Running one of this tree's programs and reading what it said.
//
// The command-line parsing of a tool lives inside its main(), so there is
// nothing to link against and nothing to call: a suite that checks what a
// tool accepts has to start it. Three packages now do that -- the driver's
// Tools/, aes67-ravenna's announcer -- and this is the part they share, kept
// here rather than copied into each.
//
// Header-only, and deliberately small: it starts a program, waits for it,
// and hands back the exit status and everything it printed.
//
#pragma once

#include <sys/wait.h>

#include <cstdio>
#include <string>

namespace AES67 {
namespace TestSupport {

struct ToolRun {
    /// The program's exit status, or -1 when it did not exit normally.
    int status{-1};
    /// Everything it wrote, both streams together.
    std::string output;
};

/// Runs `tool` with `arguments` appended, through a shell, and collects what
/// it printed on either stream -- these programs print their usage on stdout
/// and their refusals on stderr, and reading only one of them would miss half
/// the answer.
///
/// The caller is responsible for only running invocations that exit on their
/// own: a usage message, a refused argument, an unknown option. A tool that
/// goes on to open a socket and wait would hang the suite.
inline ToolRun runTool(const std::string& tool, const std::string& arguments) {
    const std::string command = "'" + tool + "' " + arguments + " 2>&1";

    ToolRun result;
    std::FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) return result;

    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result.output += buffer;
    }

    const int closed = pclose(pipe);
    result.status = WIFEXITED(closed) ? WEXITSTATUS(closed) : -1;
    return result;
}

inline bool mentions(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

/// One program and the command line to hand it, with what it should exit
/// with. A struct rather than a tuple because doctest's INFO builds a lambda
/// around what it is given, and a structured binding cannot be captured by
/// one before C++20.
struct ToolCase {
    std::string tool;
    std::string arguments;
    int status{1};
};

} // namespace TestSupport
} // namespace AES67
