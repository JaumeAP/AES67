//
// TestNMOSSettings.cpp
// Whether this driver registers with an NMOS registry, and the one field
// nobody sets by hand: the node's UUID.
//
// The default is what matters most. Registering announces the machine to
// whatever reads the plant's registry, so a driver that starts doing it
// because it was updated is a driver that surprised its installation. Off
// is the default and every unreadable path lands back on it.
//
// The id matters second. IS-04 keys a node by its UUID; generate a new one
// each launch and the registry fills with ghosts of this driver, each one
// heartbeating until it times out.
//

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/NMOSSettings.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <string>
#include <unistd.h>

using AES67::NMOSSettings;
using AES67::NMOSSettingsManager;

namespace {

/// A file this test owns, pointed at through AES67_NMOS_CONFIG_PATH: the
/// first entry in the search order and the only seam the manager offers.
struct TempConfig {
    std::string path;

    explicit TempConfig(const std::string& contents) {
        path = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp")
             + "/aes67-nmos-test.json";
        std::ofstream out(path);
        out << contents;
        out.close();
        setenv("AES67_NMOS_CONFIG_PATH", path.c_str(), 1);
    }
    ~TempConfig() {
        unsetenv("AES67_NMOS_CONFIG_PATH");
        std::remove(path.c_str());
    }
};

}  // namespace

TEST_CASE("Off is the default, and an absent file leaves it off") {
    const NMOSSettings defaults;
    CHECK(defaults.enabled == false);
    CHECK(defaults.nodeId.empty());
    CHECK(defaults.label.empty());
    CHECK(defaults.registryOverride.empty());

    setenv("AES67_NMOS_CONFIG_PATH", "/nonexistent/aes67-nmos-absent.json", 1);
    NMOSSettingsManager manager;
    const NMOSSettings loaded = manager.load();
    unsetenv("AES67_NMOS_CONFIG_PATH");

    CHECK(loaded.enabled == false);
    CHECK(loaded.nodeId.empty());
}

TEST_CASE("A file that says so turns it on") {
    TempConfig cfg(
        "{\n"
        "  \"enabled\": true,\n"
        "  \"label\": \"Studio Mac\",\n"
        "  \"nodeId\": \"8c4a5f2e-0000-4000-8000-000000000001\",\n"
        "  \"registryOverride\": \"10.0.0.9:8010\"\n"
        "}\n");

    NMOSSettingsManager manager;
    const NMOSSettings settings = manager.load();

    CHECK(settings.enabled == true);
    CHECK(settings.label == "Studio Mac");
    CHECK(settings.nodeId == "8c4a5f2e-0000-4000-8000-000000000001");
    CHECK(settings.registryOverride == "10.0.0.9:8010");
}

TEST_CASE("Saving generates the id once and then keeps it") {
    TempConfig cfg("{}");

    NMOSSettingsManager manager;
    NMOSSettings settings;
    settings.enabled = true;
    settings.label = "Studio Mac";

    REQUIRE(manager.save(settings));
    const std::string firstId = settings.nodeId;
    CHECK(firstId.size() == 36);

    // The caller is handed the id that was persisted, not an empty field
    // it would have to read back to learn.
    const NMOSSettings reloaded = manager.load();
    CHECK(reloaded.nodeId == firstId);

    // Saving again keeps it: a new id every save is a new node every save.
    NMOSSettings again = reloaded;
    REQUIRE(manager.save(again));
    CHECK(again.nodeId == firstId);
    CHECK(manager.load().nodeId == firstId);
}

TEST_CASE("The generated id is a version 4 UUID, and a different one each time") {
    std::set<std::string> seen;
    for (int i = 0; i < 100; i++) {
        const std::string id = NMOSSettingsManager::generateNodeId();
        REQUIRE(id.size() == 36);
        CHECK(id[8] == '-');
        CHECK(id[13] == '-');
        CHECK(id[18] == '-');
        CHECK(id[23] == '-');
        CHECK(id[14] == '4');                       // version
        CHECK(std::string("89ab").find(id[19]) != std::string::npos); // variant
        seen.insert(id);
    }
    CHECK(seen.size() == 100);
}

TEST_CASE("A label with a quote in it survives the round trip") {
    TempConfig cfg("{}");

    NMOSSettingsManager manager;
    NMOSSettings settings;
    settings.label = "Studio \"B\"";
    REQUIRE(manager.save(settings));
    CHECK(manager.load().label == "Studio \"B\"");
}

TEST_CASE("A malformed file is the defaults, not a half-read node") {
    TempConfig cfg("this is not json at all");

    NMOSSettingsManager manager;
    const NMOSSettings settings = manager.load();

    CHECK(settings.enabled == false);
    CHECK(settings.nodeId.empty());
}
