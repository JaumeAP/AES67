//
// Config.cpp
// AES67 macOS Driver - Build #3
// Configuration management with build tracking
//

#include "Config.hpp"
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace AES67 {

ConfigManager& ConfigManager::getInstance() {
    static ConfigManager instance;
    return instance;
}

ConfigManager::ConfigManager() {
    loadDefault();
}

bool ConfigManager::load(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }

    // Simple JSON-like parsing (would use a library in production)
    // For now, just load defaults
    return true;
}

bool ConfigManager::save(const std::string& path) const {
    ensureDirectoryExists(configDir_);

    std::ofstream file(path);
    if (!file.is_open()) {
        return false;
    }

    // Simple JSON output
    file << "{\n";
    file << "  \"deviceName\": \"" << deviceConfig_.deviceName << "\",\n";
    file << "  \"sampleRate\": " << deviceConfig_.sampleRate << ",\n";
    file << "  \"bufferSize\": " << deviceConfig_.bufferSize << ",\n";
    file << "  \"ptpEnabled\": " << (deviceConfig_.ptpEnabled ? "true" : "false") << "\n";
    file << "}\n";

    return true;
}

bool ConfigManager::loadDefault() {
    deviceConfig_ = DeviceConfig();  // Use defaults from struct
    return true;
}

std::string ConfigManager::getConfigPath() const {
    return configDir_ + "/config.json";
}

std::string ConfigManager::getMappingsPath() const {
    return configDir_ + "/mappings.json";
}

std::string ConfigManager::getLogsPath() const {
    return configDir_ + "/logs";
}

std::string ConfigManager::getBuildVersion() const {
    return readVersionFile();
}

int ConfigManager::getBuildNumber() const {
    std::string version = readVersionFile();
    // Parse "1.0.0-build.N" to extract N
    size_t buildPos = version.find("build.");
    if (buildPos == std::string::npos) {
        return 0;
    }

    std::string buildStr = version.substr(buildPos + 6);
    try {
        return std::stoi(buildStr);
    } catch (...) {
        return 0;
    }
}

void ConfigManager::incrementBuildNumber() {
    int current = getBuildNumber();
    std::string newVersion = "1.0.0-build." + std::to_string(current + 1);
    writeVersionFile(newVersion);
}

bool ConfigManager::ensureDirectoryExists(const std::string& path) const {
    struct stat info;
    if (stat(path.c_str(), &info) != 0) {
        // Directory doesn't exist, create it
        return mkdir(path.c_str(), 0755) == 0;
    }
    return S_ISDIR(info.st_mode);
}

std::string ConfigManager::readVersionFile() const {
    // Try to read VERSION.txt from project root
    std::ifstream file(versionFile_);
    if (!file.is_open()) {
        return "1.0.0-build.0";
    }

    std::string version;
    std::getline(file, version);

    // Trim whitespace
    size_t start = version.find_first_not_of(" \t\n\r");
    size_t end = version.find_last_not_of(" \t\n\r");

    if (start == std::string::npos) {
        return "1.0.0-build.0";
    }

    return version.substr(start, end - start + 1);
}

bool ConfigManager::writeVersionFile(const std::string& version) const {
    std::ofstream file(versionFile_);
    if (!file.is_open()) {
        return false;
    }

    file << version << "\n";
    return true;
}

} // namespace AES67
