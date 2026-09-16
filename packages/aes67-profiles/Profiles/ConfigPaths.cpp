#include "Profiles/ConfigPaths.h"
#include "Profiles/ProfileLog.h"

#include <cstdlib>
#include <filesystem>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

namespace AES67 {

namespace {

// The one directory name every one of these files has always used. It is
// spelled the same in all seven copies this replaces, which is the only
// reason they interoperated.
constexpr const char* kSupportDirectory = "/Library/Application Support/AES67Driver/";

} // namespace

std::vector<std::string> configSearchPaths(const char* envVar, const std::string& fileName,
                                           bool systemBeforeHome) {
    std::vector<std::string> paths;

    if (envVar != nullptr) {
        const char* envPath = std::getenv(envVar);
        if (envPath && envPath[0] != '\0') paths.emplace_back(envPath);
    }

    const std::string systemPath = std::string(kSupportDirectory) + fileName;
    if (systemBeforeHome) paths.push_back(systemPath);

    const char* home = std::getenv("HOME");
    if (!home) {
        const struct passwd* pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }
    if (home && home[0] != '\0') {
        paths.push_back(std::string(home) + kSupportDirectory + fileName);
    }

    if (!systemBeforeHome) paths.push_back(systemPath);
    return paths;
}

std::string findExistingConfig(const char* envVar, const std::string& fileName,
                               bool systemBeforeHome) {
    for (const auto& path : configSearchPaths(envVar, fileName, systemBeforeHome)) {
        struct stat st;
        if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) return path;
    }
    return "";
}

namespace {

/// Whether this process could create `filePath` without being told no.
bool couldWrite(const std::string& filePath) {
    // std::filesystem::path::parent_path() over hand-rolled find_last_of/
    // substr/resize surgery: it already normalizes what the manual walk did
    // not -- a trailing slash, a repeated one, "." and ".." components --
    // and this file already includes <filesystem> and calls
    // std::filesystem::create_directories() a few lines below, in
    // ensureParentDirectory(). ::access() stays rather than a
    // filesystem::status() permission bit: POSIX mode bits do not reliably
    // reflect what this effective uid can do (ACLs, some mounted
    // filesystems), which is exactly what access() asks the kernel directly.
    std::filesystem::path dir = std::filesystem::path(filePath).parent_path();

    // A bare file name has no parent component at all -- configSearchPaths()'s
    // own contract says an override is taken whole and names a file, not a
    // directory, and it resolves relative to the current directory the same
    // way std::ifstream(filePath) would read it.
    if (dir.empty()) dir = ".";

    // Up to the first component that exists: that is the one that has to be
    // writable, because everything under it would be created.
    for (;;) {
        std::error_code existsEc;
        if (std::filesystem::exists(dir, existsEc) && !existsEc) {
            std::error_code dirEc;
            return std::filesystem::is_directory(dir, dirEc) && !dirEc &&
                   ::access(dir.c_str(), W_OK) == 0;
        }
        const std::filesystem::path parent = dir.parent_path();
        if (parent == dir) break; // reached the root without finding one
        dir = parent;
    }
    return false;
}

} // namespace

std::string firstWritableConfigPath(const char* envVar, const std::string& fileName,
                                    bool systemBeforeHome) {
    const std::vector<std::string> paths = configSearchPaths(envVar, fileName, systemBeforeHome);
    for (const auto& path : paths) {
        if (couldWrite(path)) return path;
    }

    // None of them: hand back the first anyway, so the caller behaves as it
    // did before this function existed and the refusal surfaces where it
    // always did, at the save.
    return paths.empty() ? std::string{} : paths.front();
}

bool ensureParentDirectory(const std::string& filePath, const char* who) {
    const size_t lastSlash = filePath.find_last_of('/');
    if (lastSlash == std::string::npos) return false;
    const std::string dir = filePath.substr(0, lastSlash);

    struct stat st;
    if (stat(dir.c_str(), &st) == 0) return S_ISDIR(st.st_mode);

    // ProfileLog may compile the log line below to nothing, and then `who`
    // is a parameter with no reader; it is still the right parameter.
    (void)who;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        AES67_LOGF("%s: Failed to create directory '%s': %s",
                   who ? who : "ConfigPaths", dir.c_str(), ec.message().c_str());
        return false;
    }
    chmod(dir.c_str(), 0755);
    return true;
}

} // namespace AES67
