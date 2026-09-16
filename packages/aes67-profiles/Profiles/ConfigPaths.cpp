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
    const size_t lastSlash = filePath.find_last_of('/');
    if (lastSlash == std::string::npos) return false;

    // Up to the first component that exists: that is the one that has to be
    // writable, because everything under it would be created.
    std::string dir = filePath.substr(0, lastSlash);
    while (!dir.empty()) {
        struct stat st;
        if (stat(dir.c_str(), &st) == 0) {
            return S_ISDIR(st.st_mode) && ::access(dir.c_str(), W_OK) == 0;
        }
        const size_t slash = dir.find_last_of('/');
        if (slash == std::string::npos || slash == 0) break;
        dir.resize(slash);
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
