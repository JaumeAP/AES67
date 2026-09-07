//
// NMOSSettings.cpp
// AES67 macOS Driver
//

#include "NMOSSettings.h"
#include "../Driver/DebugLog.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <pwd.h>
#include <random>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace AES67 {

namespace {

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

std::string extractString(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return {};
    pos = json.find('"', pos);
    if (pos == std::string::npos) return {};
    ++pos;
    std::string value;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) ++pos;
        value.push_back(json[pos]);
        ++pos;
    }
    return value;
}

bool extractBool(const std::string& json, const std::string& key, bool& out) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return false;
    const size_t trueAt = json.find("true", pos);
    const size_t falseAt = json.find("false", pos);
    const size_t lineEnd = json.find('\n', pos);
    if (trueAt != std::string::npos && (lineEnd == std::string::npos || trueAt < lineEnd)) {
        out = true;
        return true;
    }
    if (falseAt != std::string::npos && (lineEnd == std::string::npos || falseAt < lineEnd)) {
        out = false;
        return true;
    }
    return false;
}

} // namespace

NMOSSettingsManager::NMOSSettingsManager() {
    std::string existing = findExistingConfig();
    if (!existing.empty()) {
        configPath_ = existing;
    } else {
        configPath_ = "/Library/Application Support/AES67Driver/" + std::string(kDefaultConfigFile);
    }
}

NMOSSettingsManager::~NMOSSettingsManager() = default;

std::string NMOSSettingsManager::getConfigPath() const { return configPath_; }

std::vector<std::string> NMOSSettingsManager::getConfigSearchPaths() {
    std::vector<std::string> paths;

    const char* envPath = std::getenv("AES67_NMOS_CONFIG_PATH");
    if (envPath && envPath[0] != '\0') paths.push_back(envPath);

    const char* home = std::getenv("HOME");
    if (!home) {
        struct passwd* pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }
    if (home && home[0] != '\0') {
        paths.push_back(std::string(home) + "/Library/Application Support/AES67Driver/" +
                        kDefaultConfigFile);
    }

    paths.push_back("/Library/Application Support/AES67Driver/" + std::string(kDefaultConfigFile));
    return paths;
}

std::string NMOSSettingsManager::findExistingConfig() {
    for (const auto& path : getConfigSearchPaths()) {
        struct stat st;
        if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) return path;
    }
    return "";
}

bool NMOSSettingsManager::ensureConfigDirectoryExists() {
    const size_t lastSlash = configPath_.find_last_of('/');
    if (lastSlash == std::string::npos) return false;
    const std::string dir = configPath_.substr(0, lastSlash);

    struct stat st;
    if (stat(dir.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
    return mkdir(dir.c_str(), 0755) == 0;
}

std::string NMOSSettingsManager::generateNodeId() {
    // A version 4 UUID from the system's random source. IS-04 only asks
    // for a UUID; what matters is that two machines never pick the same
    // one and that this machine keeps the one it picked.
    std::random_device source;
    std::uniform_int_distribution<uint32_t> byte(0, 255);

    uint8_t bytes[16];
    for (uint8_t& b : bytes) b = static_cast<uint8_t>(byte(source));
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0F) | 0x40); // version 4
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3F) | 0x80); // variant 1

    char text[37];
    (void)std::snprintf(text, sizeof(text),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
                  bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14],
                  bytes[15]);
    return std::string(text);
}

NMOSSettings NMOSSettingsManager::load() {
    NMOSSettings settings; // defaults: disabled, no id yet

    std::ifstream file(configPath_);
    if (!file.is_open()) return settings;

    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string json = buffer.str();

    bool enabled = false;
    if (extractBool(json, "enabled", enabled)) settings.enabled = enabled;
    settings.label = extractString(json, "label");
    settings.nodeId = extractString(json, "nodeId");
    settings.registryOverride = extractString(json, "registryOverride");

    AES67_LOGF("NMOSSettingsManager: Loaded from %s (enabled=%s, nodeId=%s)",
               configPath_.c_str(), settings.enabled ? "true" : "false",
               settings.nodeId.empty() ? "(none)" : settings.nodeId.c_str());
    return settings;
}

bool NMOSSettingsManager::save(NMOSSettings& settings) {
    if (!ensureConfigDirectoryExists()) {
        AES67_LOG("NMOSSettingsManager: Failed to create config directory");
        return false;
    }

    // The id is generated here rather than at registration time so that
    // what is persisted and what is announced are the same thing.
    if (settings.nodeId.empty()) settings.nodeId = generateNodeId();

    std::ostringstream json;
    json << "{\n";
    json << "  \"version\": \"1.0\",\n";
    json << "  \"enabled\": " << (settings.enabled ? "true" : "false") << ",\n";
    json << "  \"label\": \"" << jsonEscape(settings.label) << "\",\n";
    json << "  \"nodeId\": \"" << jsonEscape(settings.nodeId) << "\",\n";
    json << "  \"registryOverride\": \"" << jsonEscape(settings.registryOverride) << "\"\n";
    json << "}\n";

    std::ofstream file(configPath_);
    if (!file.is_open()) {
        AES67_LOGF("NMOSSettingsManager: Failed to open %s for writing", configPath_.c_str());
        return false;
    }
    file << json.str();
    return file.good();
}

} // namespace AES67
