#include "AmplifierUnitSettings.h"
#include "Profiles/ConfigPaths.h"
#include "../Driver/DebugLog.h"

#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>

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
    return configSearchPaths("AES67_AMPLIFIER_UNIT_CONFIG_PATH", kDefaultConfigFile);
}

std::string AmplifierUnitSettingsManager::findExistingConfig() {
    return AES67::findExistingConfig("AES67_AMPLIFIER_UNIT_CONFIG_PATH", kDefaultConfigFile);
}

bool AmplifierUnitSettingsManager::ensureConfigDirectoryExists() {
    return ensureParentDirectory(configPath_, "AmplifierUnitSettingsManager");
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

    // Optional per-unit chain sizes: "chainUnitChannels": [16, 32, 24].
    std::smatch arrMatch;
    std::regex arrPattern("\"chainUnitChannels\"\\s*:\\s*\\[([^\\]]*)\\]");
    if (std::regex_search(json, arrMatch, arrPattern) && arrMatch.size() > 1) {
        const std::string body = arrMatch[1].str();
        std::regex num("\\d+");
        for (auto it = std::sregex_iterator(body.begin(), body.end(), num);
             it != std::sregex_iterator(); ++it) {
            parsed.chainUnitChannels.push_back(static_cast<uint32_t>(std::stoul(it->str())));
        }
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

PlayoutDelaySettings AmplifierUnitSettingsManager::loadPlayoutDelay() {
    PlayoutDelaySettings settings;

    std::ifstream file(configPath_);
    if (!file.is_open()) return settings;

    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string json = buffer.str();

    std::regex pattern("\"playoutDelaySamples\"\\s*:\\s*(\\d+)");
    std::smatch match;
    if (std::regex_search(json, match, pattern) && match.size() > 1) {
        PlayoutDelaySettings parsed;
        parsed.samples = static_cast<uint32_t>(std::stoul(match[1].str()));
        if (!parsed.isValid()) {
            AES67_LOGF("AmplifierUnitSettingsManager: playout delay %u samples is out of range "
                       "(max %u) — using the receiver's own default instead",
                       parsed.samples, PlayoutDelaySettings::kMaxSamples);
            return settings;
        }
        return parsed;
    }
    return settings;
}

namespace {
std::string chainArrayJson(const std::vector<uint32_t>& chain) {
    std::ostringstream a;
    a << "[";
    for (size_t i = 0; i < chain.size(); ++i) {
        if (i) a << ", ";
        a << chain[i];
    }
    a << "]";
    return a.str();
}
} // namespace

bool AmplifierUnitSettingsManager::savePlayoutDelay(const PlayoutDelaySettings& settings) {
    if (!settings.isValid()) return false;
    // Both values live in one file, so read the other one back before
    // rewriting it rather than clobbering it.
    const AmplifierUnitSettings unit = load();
    if (!ensureConfigDirectoryExists()) return false;

    std::ostringstream json;
    json << "{\n";
    json << "  \"version\": \"1.0\",\n";
    json << "  \"unitIndex\": " << unit.unitIndex << ",\n";
    json << "  \"chainUnitChannels\": " << chainArrayJson(unit.chainUnitChannels) << ",\n";
    json << "  \"playoutDelaySamples\": " << settings.samples << "\n";
    json << "}\n";

    std::ofstream file(configPath_);
    if (!file.is_open()) return false;
    file << json.str();
    return true;
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

    const PlayoutDelaySettings delay = loadPlayoutDelay(); // preserve the other value in this file

    std::ostringstream json;
    json << "{\n";
    json << "  \"version\": \"1.0\",\n";
    json << "  \"unitIndex\": " << settings.unitIndex << ",\n";
    json << "  \"chainUnitChannels\": " << chainArrayJson(settings.chainUnitChannels) << ",\n";
    json << "  \"playoutDelaySamples\": " << delay.samples << "\n";
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
