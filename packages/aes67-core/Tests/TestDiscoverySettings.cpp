//
// TestDiscoverySettings.cpp
// AES67 Core
//
// The one switch that separates being findable from being formatted.
//
// A compatibility profile decides both today: what the audio may look like,
// and which discovery is spoken. The first is the profile's business; the
// second stops being it in a plant with two ecosystems on one switch, where a
// room has to be visible to a Dante controller and an NMOS one at once. This
// is the override, and what the tests hold it to is that it only ever widens:
// a profile's own routes are not something a settings file may switch off.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/DiscoverySettings.h"
#include "Profiles/CompatibilityProfile.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

using namespace AES67;

namespace {

/// A settings file of our own, so the test never reads or writes the one a
/// real installation has.
struct TempConfig {
    std::string path;

    explicit TempConfig(const std::string& contents) {
        path = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
               "aes67-discovery-test.json";
        if (!contents.empty()) {
            std::ofstream file(path);
            file << contents;
        }
        setenv(DiscoverySettingsManager::kConfigPathEnvVar, path.c_str(), 1);
    }

    ~TempConfig() {
        std::remove(path.c_str());
        unsetenv(DiscoverySettingsManager::kConfigPathEnvVar);
    }
};

/// What AES67Device::Initialize() computes from the two of them.
struct EffectiveDiscovery {
    bool sap, dnsSdRtsp, nmos;
};

EffectiveDiscovery effective(const CompatibilityProfile& profile,
                             const DiscoverySettings& settings) {
    return {profile.usesSap || settings.runEveryRoute,
            profile.usesDnsSdRtsp || settings.runEveryRoute,
            profile.usesNmos || settings.runEveryRoute};
}

} // namespace

TEST_CASE("With no settings file at all, discovery is whatever the profile says") {
    TempConfig config("");   // env var points at a file that does not exist
    const DiscoverySettings settings = DiscoverySettingsManager().load();
    CHECK_FALSE(settings.runEveryRoute);

    const auto dolby = CompatibilityProfile::forKind(CompatibilityProfileKind::Dolby);
    const EffectiveDiscovery dolbyRuns = effective(dolby, settings);
    CHECK_FALSE(dolbyRuns.sap);
    CHECK_FALSE(dolbyRuns.dnsSdRtsp);
    CHECK_FALSE(dolbyRuns.nmos);

    const auto dante = CompatibilityProfile::forKind(CompatibilityProfileKind::Dante);
    const EffectiveDiscovery danteRuns = effective(dante, settings);
    CHECK(danteRuns.sap);
    CHECK_FALSE(danteRuns.dnsSdRtsp);
    CHECK_FALSE(danteRuns.nmos);
}

TEST_CASE("Switched on, every profile is findable every way") {
    TempConfig config("{\n  \"runEveryRoute\": true\n}\n");
    const DiscoverySettings settings = DiscoverySettingsManager().load();
    REQUIRE(settings.runEveryRoute);

    for (const auto& profile : CompatibilityProfile::all()) {
        INFO(profile.displayName);
        const EffectiveDiscovery runs = effective(profile, settings);
        CHECK(runs.sap);
        CHECK(runs.dnsSdRtsp);
        CHECK(runs.nmos);
    }
}

TEST_CASE("It only ever widens") {
    // The switch off must not take away a route the profile asked for: that
    // is the difference between an installation choosing to be more visible
    // and a settings file quietly breaking the gear it was bought for.
    const DiscoverySettings off;
    for (const auto& profile : CompatibilityProfile::all()) {
        INFO(profile.displayName);
        const EffectiveDiscovery runs = effective(profile, off);
        if (profile.usesSap) CHECK(runs.sap);
        if (profile.usesDnsSdRtsp) CHECK(runs.dnsSdRtsp);
        if (profile.usesNmos) CHECK(runs.nmos);
    }
}

TEST_CASE("What is written is what is read back") {
    TempConfig config("");
    DiscoverySettingsManager manager;

    DiscoverySettings settings;
    settings.runEveryRoute = true;
    REQUIRE(manager.save(settings));

    CHECK(DiscoverySettingsManager().load().runEveryRoute);

    settings.runEveryRoute = false;
    REQUIRE(manager.save(settings));
    CHECK_FALSE(DiscoverySettingsManager().load().runEveryRoute);
}

TEST_CASE("A file that is not what it should be is the defaults, not a crash") {
    for (const char* contents : {"", "{", "not json at all",
                                 "{\"runEveryRoute\": \"yes please\"}",
                                 "{\"somethingElse\": true}"}) {
        TempConfig config(std::string(contents).empty() ? std::string("   ") : contents);
        CHECK_FALSE(DiscoverySettingsManager().load().runEveryRoute);
    }
}
