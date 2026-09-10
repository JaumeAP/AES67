#include "DeviceActivation.h"
#include "Profiles/ConfigPaths.h"
#include "Driver/DebugLog.h"

#include <cstdlib>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>

namespace AES67 {

namespace {

/// Reads "name": true / false out of a flat JSON object. Same shape of parse
/// PTPMasterSettings uses, and for the same reason: one boolean does not earn
/// a JSON library the driver would otherwise not link.
std::optional<bool> extractBoolField(const std::string& json, const std::string& name) {
    const std::regex pattern("\"" + name + "\"\\s*:\\s*(true|false)");
    std::smatch match;
    if (!std::regex_search(json, match, pattern)) return std::nullopt;
    return match[1].str() == "true";
}

} // namespace

DeviceActivationManager::DeviceActivationManager() {
    // An explicit path is where the file goes, whether or not it is there
    // yet: the override has to name the file to be written, not only one to
    // be found. Without this, saving through an override falls back to
    // /Library and fails for anyone who is not root.
    const char* envPath = std::getenv("AES67_DEVICE_ACTIVATION_PATH");
    if (envPath && envPath[0] != '\0') {
        configPath_ = envPath;
        return;
    }

    std::string existing = findExistingConfig();
    if (!existing.empty()) {
        configPath_ = existing;
    } else {
        configPath_ = "/Library/Application Support/AES67Driver/" + std::string(kDefaultConfigFile);
    }
}

DeviceActivationManager::~DeviceActivationManager() = default;

const std::string& DeviceActivationManager::getConfigPath() const { return configPath_; }

std::vector<std::string> DeviceActivationManager::getConfigSearchPaths() {
    return configSearchPaths("AES67_DEVICE_ACTIVATION_PATH", kDefaultConfigFile, true);
}

std::string DeviceActivationManager::findExistingConfig() {
    return AES67::findExistingConfig("AES67_DEVICE_ACTIVATION_PATH", kDefaultConfigFile, true);
}

bool DeviceActivationManager::ensureConfigDirectoryExists() {
    return ensureParentDirectory(configPath_, "DeviceActivationManager");
}

DeviceActivation DeviceActivationManager::load() {
    DeviceActivation activation; // default: active

    std::ifstream file(configPath_);
    if (!file.is_open()) return activation;

    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string json = buffer.str();

    if (auto v = extractBoolField(json, "active")) activation.active = *v;

    AES67_LOGF("DeviceActivationManager: Loaded from %s (active=%s)",
               configPath_.c_str(), activation.active ? "true" : "false");
    return activation;
}

bool DeviceActivationManager::save(const DeviceActivation& activation) {
    if (!ensureConfigDirectoryExists()) {
        AES67_LOG("DeviceActivationManager: Failed to create config directory");
        return false;
    }

    std::ofstream file(configPath_);
    if (!file.is_open()) {
        AES67_LOGF("DeviceActivationManager: Failed to open %s for writing", configPath_.c_str());
        return false;
    }

    file << "{\n  \"active\": " << (activation.active ? "true" : "false") << "\n}\n";
    AES67_LOGF("DeviceActivationManager: Saved to %s", configPath_.c_str());
    return true;
}

} // namespace AES67
