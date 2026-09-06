//
// TestDeviceActivationPlugIn.cpp
// AES67 macOS Driver
//
// The one thing the activation flag exists to do: decide whether the plug-in
// Core Audio builds carries a device or none. TestDeviceActivation.cpp covers
// reading the file; this covers acting on it, which is the part that would
// still be broken if the reader were perfect and PlugInMain ignored it.
//
// This builds a real AES67Plugin against libASPL, in this process. It does not
// talk to coreaudiod and installs nothing: aspl::Plugin is an ordinary object
// until Core Audio is handed a driver built around it, and GetDeviceCount() is
// what the host would go on to ask.
//
// Labelled `integration` because it drags in libASPL and CoreAudio, not
// because it needs a machine in any particular state.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "Driver/AES67Plugin.h"

#include <aspl/Context.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace AES67;

namespace {

/// An activation file in a temporary directory, pointed at by
/// AES67_DEVICE_ACTIVATION_PATH for as long as this object lives.
class ScopedActivation {
public:
    explicit ScopedActivation(const std::string& name, const char* contents) {
        dir_ = std::filesystem::temp_directory_path() /
               ("aes67-plugin-" + name + "-" + std::to_string(::getpid()));
        std::filesystem::create_directories(dir_);
        path_ = (dir_ / "device_active.json").string();
        if (contents) {
            std::ofstream file(path_);
            file << contents;
        }
        ::setenv("AES67_DEVICE_ACTIVATION_PATH", path_.c_str(), 1);
    }

    ~ScopedActivation() {
        ::unsetenv("AES67_DEVICE_ACTIVATION_PATH");
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

private:
    std::filesystem::path dir_;
    std::string path_;
};

UInt32 devicesPublished() {
    auto context = std::make_shared<aspl::Context>();
    AES67Plugin plugin(context);
    return plugin.GetDeviceCount();
}

} // namespace

TEST_CASE("Deactivated publishes no device") {
    ScopedActivation scoped("off", "{\n  \"active\": false\n}\n");

    // Installed, loaded, and carrying nothing: this is what "inactive" means.
    // The bundle is still in the HAL and the plug-in still constructs.
    CHECK(devicesPublished() == 0);
}

TEST_CASE("Activated publishes the device") {
    ScopedActivation scoped("on", "{\n  \"active\": true\n}\n");

    CHECK(devicesPublished() == 1);
}

TEST_CASE("No file publishes the device") {
    ScopedActivation scoped("absent", nullptr);

    // A driver installed before the flag existed has nothing to read and must
    // keep working. Getting this default backwards would make every existing
    // installation lose its device on upgrade.
    CHECK(devicesPublished() == 1);
}
