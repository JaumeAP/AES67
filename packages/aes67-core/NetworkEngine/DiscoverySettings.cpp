//
// DiscoverySettings.cpp
// AES67 Core
//

#include "NetworkEngine/DiscoverySettings.h"
#include "NetworkEngine/JsonFields.h"

#include "Profiles/ConfigPaths.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace AES67 {

namespace {

/// The value of a JSON boolean field, if the file has one. Hand-read like
/// every other settings file in this tree: the files are three lines long and
/// a JSON dependency for them would be the larger decision.

} // namespace

DiscoverySettingsManager::DiscoverySettingsManager() {
    // /Library ahead of the per-user copy, for the same reason the device
    // activation flag does it: this is written through an administrator
    // prompt and read by a driver constructed inside coreaudiod, whose HOME
    // is not the logged-in user's. A stray copy under some home directory
    // must not decide whether a room is findable.
    constexpr bool kSystemBeforeHome = true;
    configPath_ = findExistingConfig(kConfigPathEnvVar, kDefaultConfigFile, kSystemBeforeHome);
    if (configPath_.empty()) {
        // Not the system-wide path unconditionally: coreaudiod runs as
        // _coreaudiod and cannot create it, and neither can a tool or a
        // test. The first of the same search paths this process could
        // actually write, in the same order it already searches them.
        configPath_ = firstWritableConfigPath(kConfigPathEnvVar, kDefaultConfigFile, kSystemBeforeHome);
    }
}

DiscoverySettings DiscoverySettingsManager::load() {
    DiscoverySettings settings;

    std::ifstream file(configPath_);
    if (!file.is_open()) return settings;

    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string json = buffer.str();

    if (auto value = extractBoolField(json, "runEveryRoute")) settings.runEveryRoute = *value;
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
