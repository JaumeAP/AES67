//
// DiscoverySettings.cpp
// AES67 Core
//

#include "NetworkEngine/DiscoverySettings.h"

#include "Profiles/ConfigPaths.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace AES67 {

namespace {

/// The value of a JSON boolean field, if the file has one. Hand-read like
/// every other settings file in this tree: the files are three lines long and
/// a JSON dependency for them would be the larger decision.
bool extractBool(const std::string& json, const std::string& key, bool& out) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return false;
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    if (json.compare(pos, 4, "true") == 0) { out = true; return true; }
    if (json.compare(pos, 5, "false") == 0) { out = false; return true; }
    return false;
}

} // namespace

DiscoverySettingsManager::DiscoverySettingsManager() {
    configPath_ = findExistingConfig(kConfigPathEnvVar, kDefaultConfigFile);
    if (configPath_.empty()) {
        const std::vector<std::string> paths =
            configSearchPaths(kConfigPathEnvVar, kDefaultConfigFile);
        configPath_ = paths.empty()
            ? std::string("/Library/Application Support/AES67Driver/") + kDefaultConfigFile
            : paths.front();
    }
}

DiscoverySettings DiscoverySettingsManager::load() {
    DiscoverySettings settings;

    std::ifstream file(configPath_);
    if (!file.is_open()) return settings;

    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string json = buffer.str();

    extractBool(json, "runEveryRoute", settings.runEveryRoute);
    return settings;
}

bool DiscoverySettingsManager::save(const DiscoverySettings& settings) {
    if (!ensureParentDirectory(configPath_, "DiscoverySettings")) return false;

    std::ofstream file(configPath_);
    if (!file.is_open()) return false;

    file << "{\n";
    file << "  \"runEveryRoute\": " << (settings.runEveryRoute ? "true" : "false") << "\n";
    file << "}\n";
    return file.good();
}

} // namespace AES67
