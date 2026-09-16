//
// CheckReport.h
// AES67 core
// The [OK] / [XX] / [??] report the interop programs print, and the two
// counts behind it.
//
// Three programs printed it -- Tools/AES67InteropSim, Tools/DanteInteropSim
// and Tools/AES67LiveDaemonInterop -- with the same two functions and the
// same two counters written out in each, under three pairs of names: ok and
// check for the same thing, unsettled and observe for the other. One of the
// three did not count the unsettled claims at all, so its summary could not
// say how many questions it had left open.
//
// The counters are namespace-scope and mutable, which is what they already
// were: each of these is one translation unit with a main(), the checks are
// spread across its free functions, and threading a report object through
// them would be ceremony for no reader's benefit.
//
// [??] is not a failure and never has been. A claim a program cannot settle
// -- because settling it needs a live Dante Controller, or a daemon whose
// receive path is a kernel module -- is worth saying out loud and worth not
// pretending to have answered. Only `failedCount` decides the exit status.
//
#pragma once

#include <cstdio>
#include <string>

namespace AES67 {
namespace CheckReport {

/// Claims that did not hold. The exit status of all three programs.
inline int failedCount = 0;

/// Claims printed and left open, which is a different thing from a claim
/// that failed.
inline int unsettledCount = 0;

/// One claim, and whether it holds.
inline void check(const char* what, bool condition, const std::string& detail) {
    std::printf("  [%s] %s -- %s\n", condition ? "OK" : "XX", what, detail.c_str());
    if (!condition) ++failedCount;
}

/// A claim this program cannot settle: printed, counted, never failed.
inline void unsettled(const char* what, const std::string& detail) {
    std::printf("  [??] %s -- %s\n", what, detail.c_str());
    ++unsettledCount;
}

} // namespace CheckReport
} // namespace AES67
