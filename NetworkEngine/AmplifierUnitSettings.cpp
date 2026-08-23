#include "AmplifierUnitSettings.h"
#include "../Driver/DebugLog.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <pwd.h>
#include <regex>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace AES67 {

AmplifierUnitSettingsManager::AmplifierUnitSettingsManager() {
    std::string existing = findExistingConfig();
    configPath_ = existing.empty()
        ? "/Library/Application Support/AES67Driver/" + std::string(kDefaultConfigFile)
        : existing;
}

AmplifierUnitSettingsManager::~AmplifierUnitSettingsManager() = default;

std::string AmplifierUnitSettingsManager::getConfigPath() const { return configPath_; }

std::vector<std::string> AmplifierUnitSettingsManager::getConfigSearchPaths() {
    std::vector<std::string> paths;

    const char* envPath = std::getenv("AES67_AMPLIFIER_UNIT_CONFIG_PATH");
    if (envPath && envPath[0] != '\0') paths.push_back(envPath);

    const char* home = std::getenv("HOME");
    if (!home) {
        struct passwd* pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }
    if (home && home[0] != '\0') {
        paths.push_back(std::string(home) + "/Library/Application Support/AES67Driver/" + kDefaultConfigFile);
    }

    paths.push_back("/Library/Application Support/AES67Driver/" + std::string(kDefaultConfigFile));
    return paths;
}

std::string AmplifierUnitSettingsManager::findExistingConfig() {
    for (const auto& path : getConfigSearchPaths()) {
        struct stat st;
        if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) return path;
    }
    return "";
}

bool AmplifierUnitSettingsManager::ensureConfigDirectoryExists() {
    size_t lastSlash = configPath_.find_last_of('/');
    if (lastSlash == std::string::npos) return false;
    std::string dir = configPath_.substr(0, lastSlash);

    struct stat st;
    if (stat(dir.c_str(), &st) == 0) return S_ISDIR(st.st_mode);

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        AES67_LOGF("AmplifierUnitSettingsManager: Failed to create directory '%s': %s",
                   dir.c_str(), ec.message().c_str());
        return false;
    }
    chmod(dir.c_str(), 0755);
    return true;
}

AmplifierUnitSettings AmplifierUnitSettingsManager::load() {
    AmplifierUnitSettings settings; // default: unit 1

    std::ifstream file(configPath_);
    if (!file.is_open()) return settings;

    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string json = buffer.str();

    AmplifierUnitSettings parsed;
    std::regex pattern("\"unitIndex\"\\s*:\\s*(\\d+)");
    std::smatch match;
    if (std::regex_search(json, match, pattern) && match.size() > 1) {
        parsed.unitIndex = static_cast<uint32_t>(std::stoul(match[1].str()));
    }

    if (!parsed.isValid()) {
        AES67_LOGF("AmplifierUnitSettingsManager: %s holds an out-of-range unit index (%u) "
                   "— using unit 1 instead",
                   configPath_.c_str(), parsed.unitIndex);
        return settings;
    }

    AES67_LOGF("AmplifierUnitSettingsManager: Loaded from %s (unit %u)",
               configPath_.c_str(), parsed.unitIndex);
    return parsed;
}

bool AmplifierUnitSettingsManager::save(const AmplifierUnitSettings& settings) {
    if (!settings.isValid()) {
        AES67_LOGF("AmplifierUnitSettingsManager: refusing to save out-of-range unit index %u",
                   settings.unitIndex);
        return false;
    }
    if (!ensureConfigDirectoryExists()) {
        AES67_LOG("AmplifierUnitSettingsManager: Failed to create config directory");
        return false;
    }

    std::ostringstream json;
    json << "{\n";
    json << "  \"version\": \"1.0\",\n";
    json << "  \"unitIndex\": " << settings.unitIndex << "\n";
    json << "}\n";

    std::ofstream file(configPath_);
    if (!file.is_open()) {
        AES67_LOGF("AmplifierUnitSettingsManager: Failed to open %s for writing", configPath_.c_str());
        return false;
    }
    file << json.str();
    AES67_LOGF("AmplifierUnitSettingsManager: Saved to %s (unit %u)",
               configPath_.c_str(), settings.unitIndex);
    return true;
}

} // namespace AES67
