//
// ConfCheck.h
// aes67-linux-daemon
// Reading a daemon.conf back and saying what is wrong with it.
//
// Upstream's daemon reads its configuration and validates almost none of it:
// json.cpp assigns each key to a setter and a value that does not fit the
// setter's type is truncated where nobody sees it. This says what a profile
// forbids, and separately what the file says that the daemon, the standards it
// implements or its own code will not do anything useful with.
//
// Every check here answers to something written down -- a field of the
// profile, a line of upstream's source, or a standard -- and anything that is
// merely unusual is a warning, not an error.
//
#pragma once

#include "Profiles/CompatibilityProfile.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace AES67::LinuxDriver {

enum class Severity {
    Error,    ///< The daemon, a profile or a standard says this cannot work.
    Warning,  ///< It works, and is probably not what was meant.
};

struct Finding {
    Severity severity{Severity::Error};
    std::string key;      ///< The configuration key, or empty for the file itself.
    std::string message;  ///< What is wrong, in one line.
};

/// Whether a path the configuration names exists. Injected so the checks are
/// testable without a filesystem; the tool passes one that asks the disk.
using PathProbe = std::function<bool(const std::string&)>;

/// Reads `conf` -- upstream's flat daemon.conf, one key per line -- and
/// returns everything wrong with it, errors and warnings together, in the
/// order the keys appear. A profile, when given, adds the checks only a
/// profile can make.
std::vector<Finding> checkConf(const std::string& conf,
                               std::optional<CompatibilityProfileKind> kind,
                               const PathProbe& exists);

/// True when any finding is an error.
bool hasError(const std::vector<Finding>& findings);

} // namespace AES67::LinuxDriver
