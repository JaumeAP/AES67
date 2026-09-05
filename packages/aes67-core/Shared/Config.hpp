//
// Config.hpp
// AES67 macOS Driver - Build #1
// Configuration management and persistence
//

#pragma once

#include "Types.h"
#include <string>
#include <map>
#include <optional>

namespace AES67 {

//
// Configuration Manager
//
// Handles loading, saving, and accessing driver configuration
// Uses JSON for persistence
//
class ConfigManager {
public:
    static ConfigManager& getInstance();

    // Load/save configuration
    bool load(const std::string& path);
    bool save(const std::string& path) const;
    bool loadDefault();

    // Device configuration
    DeviceConfig& getDeviceConfig() { return deviceConfig_; }
    const DeviceConfig& getDeviceConfig() const { return deviceConfig_; }

    // Generic key-value access
    template<typename T>
    std::optional<T> get(const std::string& key) const;

    template<typename T>
    void set(const std::string& key, const T& value);

    // Persistence paths
    std::string getConfigPath() const;
    std::string getMappingsPath() const;
    std::string getLogsPath() const;

    // Build information
    std::string getBuildVersion() const;
    int getBuildNumber() const;
    void incrementBuildNumber();

private:
    ConfigManager();
    ~ConfigManager() = default;

    // Prevent copy/move
    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;

    DeviceConfig deviceConfig_;
    std::map<std::string, std::string> customSettings_;

    // Paths
    std::string configDir_{"/Library/Application Support/AES67Driver"};
    std::string versionFile_{"VERSION.txt"};

    // Helper methods
    bool ensureDirectoryExists(const std::string& path) const;
    std::string readVersionFile() const;
    bool writeVersionFile(const std::string& version) const;
};

} // namespace AES67
