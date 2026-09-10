//
// ConfigPaths.h
// AES67 profiles
//
// Where a settings file is looked for, and how its directory is made.
//
// Seven managers across three packages had written the same twenty lines:
// an environment variable, then ~/Library/Application Support/AES67Driver,
// then the same path under /Library, and a create_directories() with a stat
// in front of it. They agreed, which is the only reason nobody noticed --
// the day one of them stopped agreeing, a file would be written where the
// reader was not looking.
//
// It lives here, in the lowest package, because it is the only place all
// seven can reach: aes67-profiles is what everything else is allowed to
// include, and it is not allowed to include anything.
//
#pragma once

#include <string>
#include <vector>

namespace AES67 {

/// Where to look for `fileName`, in the order to look.
///
/// `envVar`, when set and not empty, comes first and is taken whole: it names
/// a file, not a directory, which is what makes it useful for a test or a
/// second instance. After it come the per-user copy under HOME -- from the
/// environment, or from the password database when the environment has none,
/// which is the case inside a launchd job -- and the system-wide one.
/// `systemBeforeHome` puts /Library ahead of the per-user copy. One caller
/// wants that and says why: a flag written through an administrator prompt,
/// read by a driver constructed inside coreaudiod, whose HOME is not the
/// logged-in user's. A stray copy under some home directory must not decide
/// whether the device appears.
std::vector<std::string> configSearchPaths(const char* envVar, const std::string& fileName,
                                           bool systemBeforeHome = false);

/// The first path from `configSearchPaths` that exists and is a regular file,
/// or "" when none of them is.
std::string findExistingConfig(const char* envVar, const std::string& fileName,
                               bool systemBeforeHome = false);

/// Makes the directory holding `filePath`, with 0755, if it is not there.
///
/// True when the directory exists afterwards. False when `filePath` names no
/// directory at all, or when it could not be created -- and in that case the
/// reason is logged through ProfileLog, prefixed with `who`.
bool ensureParentDirectory(const std::string& filePath, const char* who);

} // namespace AES67
