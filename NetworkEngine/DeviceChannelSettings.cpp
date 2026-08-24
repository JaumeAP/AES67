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
    // Every group of 8 up to the RT-buffer ceiling: 8, 16, 24, 32, ... 128.
    // This is a "usable channel" cap on top of the fixed 128-channel buffers
    // (which are unchanged and hardware-verified), NOT a change to what the
    // device advertises — so any multiple of 8 is safe, and finer than the
    // old {8,16,32,64,128} presets. It lets a detected layout of, say, 48 or
    // 96 channels (DMA32+DMA16, three DMA32) be exposed exactly instead of
    // rounded to a coarse preset.
    static const std::vector<uint32_t> counts = [] {
        std::vector<uint32_t> c;
        for (uint32_t n = kChannelGroupSize; n <= kMaxDeviceChannels; n += kChannelGroupSize) {
            c.push_back(n);
        }
        return c;
    }();
    return counts;
}

uint32_t DeviceChannelSelection::totalChannelCount() const {
    uint32_t total = channelCount;
    if (auxChannelEnabled) total += DeviceChannelSettings::kChannelGroupSize;
    return std::min(total, DeviceChannelSettings::kMaxDeviceChannels);
}

bool DeviceChannelSelection::isValid() const {
    const auto& allowed = DeviceChannelSettings::allowedChannelCounts();
    if (std::find(allowed.begin(), allowed.end(), channelCount) == allowed.end()) {
        return false;
    }
    // The auxiliary group must actually fit — at 128 it doesn't.
    if (auxChannelEnabled && channelCount + DeviceChannelSettings::kChannelGroupSize
                                  > DeviceChannelSettings::kMaxDeviceChannels) {
        return false;
    }
    return true;
}

bool DeviceChannelSettings::isValid() const {
    return rx.isValid() && tx.isValid();
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
    DeviceChannelSettings settings; // defaults: 128 channels each direction, no aux

    std::ifstream file(configPath_);
    if (!file.is_open()) return settings;

    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string json = buffer.str();

    DeviceChannelSettings parsed;
    extractUint32Field(json, "rxChannelCount", parsed.rx.channelCount);
    extractBoolField(json, "rxAuxChannelEnabled", parsed.rx.auxChannelEnabled);
    extractUint32Field(json, "txChannelCount", parsed.tx.channelCount);
    extractBoolField(json, "txAuxChannelEnabled", parsed.tx.auxChannelEnabled);

    if (!parsed.isValid()) {
        AES67_LOGF("DeviceChannelSettingsManager: %s holds an invalid combination "
                   "(rx=%u/aux=%s, tx=%u/aux=%s) — using defaults instead",
                   configPath_.c_str(), parsed.rx.channelCount,
                   parsed.rx.auxChannelEnabled ? "true" : "false",
                   parsed.tx.channelCount,
                   parsed.tx.auxChannelEnabled ? "true" : "false");
        return settings;
    }

    AES67_LOGF("DeviceChannelSettingsManager: Loaded from %s (rx=%u+%s=%u, tx=%u+%s=%u)",
               configPath_.c_str(),
               parsed.rx.channelCount, parsed.rx.auxChannelEnabled ? "8" : "0", parsed.rx.totalChannelCount(),
               parsed.tx.channelCount, parsed.tx.auxChannelEnabled ? "8" : "0", parsed.tx.totalChannelCount());
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
    json << "  \"rxChannelCount\": " << settings.rx.channelCount << ",\n";
    json << "  \"rxAuxChannelEnabled\": " << (settings.rx.auxChannelEnabled ? "true" : "false") << ",\n";
    json << "  \"txChannelCount\": " << settings.tx.channelCount << ",\n";
    json << "  \"txAuxChannelEnabled\": " << (settings.tx.auxChannelEnabled ? "true" : "false") << "\n";
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
