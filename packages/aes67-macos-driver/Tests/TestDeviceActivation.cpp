//
// TestDeviceActivation.cpp
// AES67 macOS Driver
//
// Tests for Driver/DeviceActivation.h — the one setting that decides whether
// the plug-in publishes a device at all. Every case here points
// AES67_DEVICE_ACTIVATION_PATH at a file in a temporary directory, so nothing
// touches the real ~/Library/Application Support/AES67Driver.
//
// The default matters more than the file format: a driver installed before
// this setting existed has nothing to read, and must stay active.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "Driver/DeviceActivation.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace AES67;

namespace {

/// A path inside a fresh temporary directory, removed when the test ends,
/// with AES67_DEVICE_ACTIVATION_PATH pointing at it for the lifetime of the
/// object.
class ScopedActivationPath {
public:
    explicit ScopedActivationPath(const std::string& name) {
        dir_ = std::filesystem::temp_directory_path() /
               ("aes67-activation-" + name + "-" + std::to_string(::getpid()));
        std::filesystem::create_directories(dir_);
        path_ = (dir_ / "device_active.json").string();
        ::setenv("AES67_DEVICE_ACTIVATION_PATH", path_.c_str(), 1);
    }

    ~ScopedActivationPath() {
        ::unsetenv("AES67_DEVICE_ACTIVATION_PATH");
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    const std::string& path() const { return path_; }

    void write(const std::string& contents) const {
        std::ofstream file(path_);
        file << contents;
    }

private:
    std::filesystem::path dir_;
    std::string path_;
};

} // namespace

TEST_CASE("Absent file means active") {
    ScopedActivationPath scoped("absent");

    // Nothing written: this is the driver that predates the setting.
    CHECK(DeviceActivationManager().load().active);
}

TEST_CASE("False in the file deactivates") {
    ScopedActivationPath scoped("false");
    scoped.write("{\n  \"active\": false\n}\n");

    CHECK_FALSE(DeviceActivationManager().load().active);
}

TEST_CASE("True in the file activates") {
    ScopedActivationPath scoped("true");
    scoped.write("{\n  \"active\": true\n}\n");

    CHECK(DeviceActivationManager().load().active);
}

TEST_CASE("Unreadable content leaves the default alone") {
    ScopedActivationPath scoped("garbage");
    scoped.write("this is not JSON at all");

    // A corrupt file must not take the device away; it is the same situation
    // as no file, and the safe answer is the same.
    CHECK(DeviceActivationManager().load().active);
}

TEST_CASE("A field of another name is ignored") {
    ScopedActivationPath scoped("othername");
    scoped.write("{\n  \"inactive\": false\n}\n");

    CHECK(DeviceActivationManager().load().active);
}

TEST_CASE("Round trip through save") {
    ScopedActivationPath scoped("roundtrip");

    DeviceActivation off;
    off.active = false;
    REQUIRE(DeviceActivationManager().save(off));
    CHECK_FALSE(DeviceActivationManager().load().active);

    DeviceActivation on;
    on.active = true;
    REQUIRE(DeviceActivationManager().save(on));
    CHECK(DeviceActivationManager().load().active);
}

TEST_CASE("The path the manager reports is the one it was pointed at") {
    ScopedActivationPath scoped("path");
    scoped.write("{\n  \"active\": true\n}\n");

    CHECK(DeviceActivationManager().getConfigPath() == scoped.path());
}
