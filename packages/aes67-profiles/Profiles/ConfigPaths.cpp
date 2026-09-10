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
        struct passwd* pw = getpwuid(getuid());
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
