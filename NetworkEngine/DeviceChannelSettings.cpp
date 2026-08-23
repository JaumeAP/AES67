#include "DeviceChannelSettings.h"
#include "../Driver/DebugLog.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <pwd.h>
#include <regex>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace AES67 {

const std::vector<uint32_t>& DeviceChannelSettings::allowedChannelCounts() {
    static const std::vector<uint32_t> counts = {8, 16, 32, 64, 128};
    return counts;
}

uint32_t DeviceChannelSettings::totalChannelCount() const {
    uint32_t total = channelCount;
    if (auxChannelEnabled) total += kChannelGroupSize;
    return std::min(total, kMaxDeviceChannels);
}

bool DeviceChannelSettings::isValid() const {
    const auto& allowed = allowedChannelCounts();
    if (std::find(allowed.begin(), allowed.end(), channelCount) == allowed.end()) {
        return false;
    }
    // The auxiliary group must actually fit — at 128 it doesn't.
    if (auxChannelEnabled && channelCount + kChannelGroupSize > kMaxDeviceChannels) {
        return false;
    }
    return true;
}

DeviceChannelSettingsManager::DeviceChannelSettingsManager() {
    std::string existing = findExistingConfig();
    configPath_ = existing.empty()
        ? "/Library/Application Support/AES67Driver/" + std::string(kDefaultConfigFile)
        : existing;
}

DeviceChannelSettingsManager::~DeviceChannelSettingsManager() = default;

std::string DeviceChannelSettingsManager::getConfigPath() const { return configPath_; }

std::vector<std::string> DeviceChannelSettingsManager::getConfigSearchPaths() {
    std::vector<std::string> paths;

    const char* envPath = std::getenv("AES67_DEVICE_CHANNELS_CONFIG_PATH");
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

std::string DeviceChannelSettingsManager::findExistingConfig() {
    for (const auto& path : getConfigSearchPaths()) {
        struct stat st;
        if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) return path;
    }
    return "";
}

bool DeviceChannelSettingsManager::ensureConfigDirectoryExists() {
    size_t lastSlash = configPath_.find_last_of('/');
    if (lastSlash == std::string::npos) return false;
    std::string dir = configPath_.substr(0, lastSlash);

    struct stat st;
    if (stat(dir.c_str(), &st) == 0) return S_ISDIR(st.st_mode);

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        AES67_LOGF("DeviceChannelSettingsManager: Failed to create directory '%s': %s",
                   dir.c_str(), ec.message().c_str());
        return false;
    }
    chmod(dir.c_str(), 0755);
    return true;
}

namespace {

bool extractUint32Field(const std::string& json, const std::string& key, uint32_t& out) {
    std::regex pattern("\"" + key + "\"\\s*:\\s*(\\d+)");
    std::smatch match;
    if (std::regex_search(json, match, pattern) && match.size() > 1) {
        out = static_cast<uint32_t>(std::stoul(match[1].str()));
        return true;
    }
    return false;
}

bool extractBoolField(const std::string& json, const std::string& key, bool& out) {
    std::regex pattern("\"" + key + "\"\\s*:\\s*(true|false)");
    std::smatch match;
    if (std::regex_search(json, match, pattern) && match.size() > 1) {
        out = (match[1].str() == "true");
        return true;
    }
    return false;
}

} // namespace

DeviceChannelSettings DeviceChannelSettingsManager::load() {
    DeviceChannelSettings settings; // defaults: 128 channels, no aux

    std::ifstream file(configPath_);
    if (!file.is_open()) return settings;

    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string json = buffer.str();

    DeviceChannelSettings parsed;
    extractUint32Field(json, "channelCount", parsed.channelCount);
    extractBoolField(json, "auxChannelEnabled", parsed.auxChannelEnabled);

    if (!parsed.isValid()) {
        AES67_LOGF("DeviceChannelSettingsManager: %s holds an invalid combination "
                   "(channelCount=%u, aux=%s) — using defaults instead",
                   configPath_.c_str(), parsed.channelCount,
                   parsed.auxChannelEnabled ? "true" : "false");
        return settings;
    }

    AES67_LOGF("DeviceChannelSettingsManager: Loaded from %s (channelCount=%u, aux=%s, total=%u)",
               configPath_.c_str(), parsed.channelCount,
               parsed.auxChannelEnabled ? "true" : "false", parsed.totalChannelCount());
    return parsed;
}

bool DeviceChannelSettingsManager::save(const DeviceChannelSettings& settings) {
    if (!settings.isValid()) {
        AES67_LOG("DeviceChannelSettingsManager: refusing to save an invalid combination");
        return false;
    }
    if (!ensureConfigDirectoryExists()) {
        AES67_LOG("DeviceChannelSettingsManager: Failed to create config directory");
        return false;
    }

    std::ostringstream json;
    json << "{\n";
    json << "  \"version\": \"1.0\",\n";
    json << "  \"channelCount\": " << settings.channelCount << ",\n";
    json << "  \"auxChannelEnabled\": " << (settings.auxChannelEnabled ? "true" : "false") << "\n";
    json << "}\n";

    std::ofstream file(configPath_);
    if (!file.is_open()) {
        AES67_LOGF("DeviceChannelSettingsManager: Failed to open %s for writing", configPath_.c_str());
        return false;
    }
    file << json.str();
    AES67_LOGF("DeviceChannelSettingsManager: Saved to %s", configPath_.c_str());
    return true;
}

} // namespace AES67
