//
// DiscoverySettings.h
// AES67 Core
//
// Whether this driver's discovery follows the active profile, or runs
// everything regardless of it.
//
// The compatibility profile decides two things at once: what the audio may
// look like, and which discovery is spoken. That pairing is right for a plant
// built around one ecosystem and wrong for a mixed one -- a room with Dante
// gear and an NMOS controller cannot be visible to both without adopting the
// media rules of whichever profile happens to run all three. The formats are
// the part a profile must decide; being findable is not.
//
// So this is one switch, off by default: with it off, discovery is whatever
// the profile says, which is what every build before it did. With it on, SAP,
// DNS-SD with RTSP and NMOS all run whatever the profile says, and the
// profile keeps deciding the formats alone.
//
// It never turns discovery OFF for a profile that asks for it: a profile's
// own routes always run. Widening what is heard and announced is a decision
// an installation can make; narrowing it below what the gear needs is not
// something a settings file should be able to do by accident.
//
#pragma once

#include <string>
#include <vector>

namespace AES67 {

struct DiscoverySettings {
    /// Run SAP, DNS-SD/RTSP and NMOS regardless of the active profile.
    /// False -- the default, and what every build before this did -- leaves
    /// the choice to the profile.
    bool runEveryRoute{false};
};

class DiscoverySettingsManager {
public:
    DiscoverySettingsManager();

    /// Never throws and never fails: an absent or unreadable file is the
    /// defaults, which is "follow the profile".
    DiscoverySettings load();

    bool save(const DiscoverySettings& settings);

    const std::string& getConfigPath() const { return configPath_; }

    /// The environment variable that points at a file instead, for a test or
    /// a second instance. Same shape as every other settings file here.
    static constexpr const char* kConfigPathEnvVar = "AES67_DISCOVERY_CONFIG_PATH";
    static constexpr const char* kDefaultConfigFile = "discovery.json";

private:
    std::string configPath_;
};

} // namespace AES67
