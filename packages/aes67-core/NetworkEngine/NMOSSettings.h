//
// NMOSSettings.h
// AES67 macOS Driver
//
// Whether this driver registers itself with an NMOS registry, and what it
// calls itself when it does.
//
// Off by default. Registering announces this machine to whatever reads
// the plant's registry, and that is a decision an installation makes, not
// something a driver should start doing because it was updated.
//
// The node id is the one field nobody sets by hand: IS-04 keys a node by
// its UUID, so it has to be the SAME across restarts or the registry
// fills with ghosts of this driver. It is generated once, on the first
// save, and kept.
//
#pragma once

#include <string>
#include <vector>

namespace AES67 {

struct NMOSSettings {
    /// Register with a registry at all. False is what every build before
    /// this did.
    bool enabled{false};

    /// What a controller shows for this node. Empty means the driver
    /// picks something from the hostname at registration time.
    std::string label;

    /// The node's UUID. Empty until the first save generates one.
    std::string nodeId;

    /// A registry to use instead of whatever mDNS finds, as "host:port".
    /// Empty means discover, which is the normal case; a plant with a
    /// registry on another subnet has no mDNS to find it with.
    std::string registryOverride;
};

class NMOSSettingsManager {
public:
    NMOSSettingsManager();
    ~NMOSSettingsManager();

    /// Never throws and never fails: an absent or unreadable file is the
    /// defaults, which is "do not register".
    NMOSSettings load();

    /// Writes the settings, generating a node id first if there is none.
    /// The id it generated is left in `settings` so the caller uses the
    /// same one it just persisted.
    bool save(NMOSSettings& settings);

    std::string getConfigPath() const;

    /// A random UUID, version 4. Public because the driver needs one at
    /// first run and because a generator worth trusting is worth testing.
    static std::string generateNodeId();

private:
    std::vector<std::string> getConfigSearchPaths();
    std::string findExistingConfig();
    bool ensureConfigDirectoryExists();

    std::string configPath_;
    static constexpr const char* kDefaultConfigFile = "nmos.json";
};

} // namespace AES67
