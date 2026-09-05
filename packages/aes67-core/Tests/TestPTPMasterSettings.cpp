//
// TestPTPMasterSettings.cpp
// PTP master configuration: search order, round trip, and what a missing or
// malformed file leaves behind. Had no tests.
//
// The default matters more than it looks. masterCapable is false, and that is
// the behaviour this driver had before the feature existed: slave-only. Every
// path that fails to read a setting has to land back on that, because a driver
// that decides to become a grandmaster by accident disciplines a whole network
// to the wrong clock.
//

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/PTP/PTPMasterSettings.h"

#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <fstream>
#include <string>

using AES67::PTPMasterSettings;
using AES67::PTPMasterSettingsManager;

namespace {

/// A file this test owns, pointed at through AES67_PTP_MASTER_CONFIG_PATH.
///
/// That environment variable is the first entry in the search order, and it is
/// the only seam the manager offers: its constructor resolves the path itself
/// rather than taking one. Which makes the variable a testing seam by
/// accident, and a deployment one on purpose.
struct TempConfig {
    std::string path;

    explicit TempConfig(const std::string& contents) {
        path = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp")
             + "/aes67-ptp-master-test.json";
        std::ofstream out(path);
        out << contents;
        out.close();
        setenv("AES67_PTP_MASTER_CONFIG_PATH", path.c_str(), 1);
    }
    ~TempConfig() {
        unsetenv("AES67_PTP_MASTER_CONFIG_PATH");
        std::remove(path.c_str());
    }
};

}  // namespace

TEST_CASE("The defaults are the behaviour from before the feature existed") {
    const PTPMasterSettings defaults;

    CHECK(defaults.masterCapable == false);
    CHECK(defaults.ptpEnabled == false);
    CHECK(defaults.requireLock == false);
    CHECK(defaults.clockSourceKind == "internal");
    CHECK(defaults.lockToDeviceUID.empty());
}

TEST_CASE("The environment override wins over the installed locations") {
    // getConfigSearchPaths() is private, so the order is checked through what
    // the manager actually resolves to rather than by reading the list.
    // AES67_PTP_MASTER_CONFIG_PATH is first in that order, which is what makes
    // it usable both for tests and for a deployment that keeps its config
    // somewhere else.
    TempConfig cfg(R"({ "masterCapable": true })");

    PTPMasterSettingsManager manager;
    CHECK(manager.getConfigPath() == cfg.path);
    CHECK(manager.load().masterCapable);
}

TEST_CASE("A missing file leaves the defaults untouched") {
    setenv("AES67_PTP_MASTER_CONFIG_PATH", "/nonexistent/aes67/ptp_master.json", 1);
    PTPMasterSettingsManager manager;
    const PTPMasterSettings settings = manager.load();
    unsetenv("AES67_PTP_MASTER_CONFIG_PATH");

    CHECK(settings.masterCapable == false);
    CHECK(settings.clockSourceKind == "internal");
}

TEST_CASE("Fields present in the file are read") {
    TempConfig cfg(R"({
  "masterCapable": true,
  "ptpEnabled": true,
  "requireLock": true,
  "clockSourceKind": "device",
  "lockToDeviceUID": "AppleUSBAudioEngine:Focusrite"
})");

    PTPMasterSettingsManager manager;
    const PTPMasterSettings settings = manager.load();

    CHECK(settings.masterCapable);
    CHECK(settings.ptpEnabled);
    CHECK(settings.requireLock);
    CHECK(settings.clockSourceKind == "device");
    CHECK(settings.lockToDeviceUID == "AppleUSBAudioEngine:Focusrite");
}

TEST_CASE("Fields absent from the file keep their defaults") {
    // A partial file is the normal case after adding a setting: the file on
    // disk predates it. Every missing field has to fall back rather than
    // arriving as false or empty by accident.
    TempConfig cfg(R"({ "clockSourceKind": "device" })");

    PTPMasterSettingsManager manager;
    const PTPMasterSettings settings = manager.load();

    CHECK(settings.clockSourceKind == "device");
    CHECK(settings.masterCapable == false);
    CHECK(settings.ptpEnabled == false);
    CHECK(settings.requireLock == false);
}

TEST_CASE("Saving and loading round-trips every field") {
    TempConfig cfg("{}");

    PTPMasterSettings written;
    written.masterCapable = true;
    written.ptpEnabled = true;
    written.requireLock = false;
    written.clockSourceKind = "device";
    written.lockToDeviceUID = "SomeDevice:1234";

    PTPMasterSettingsManager manager;
    REQUIRE(manager.save(written));

    const PTPMasterSettings read = manager.load();
    CHECK(read.masterCapable == written.masterCapable);
    CHECK(read.ptpEnabled == written.ptpEnabled);
    CHECK(read.requireLock == written.requireLock);
    CHECK(read.clockSourceKind == written.clockSourceKind);
    CHECK(read.lockToDeviceUID == written.lockToDeviceUID);
}

TEST_CASE("A malformed file falls back to the defaults rather than half-reading it") {
    TempConfig cfg("this is not json at all");

    PTPMasterSettingsManager manager;
    const PTPMasterSettings settings = manager.load();

    CHECK(settings.masterCapable == false);
    CHECK(settings.clockSourceKind == "internal");
}
