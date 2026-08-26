//
// PlugInMain.cpp
// AES67 macOS Driver - Build #6
// AudioServerPlugIn entry point
//

#include "AES67Device.h"
#include "Driver/DebugLog.h"
#include <aspl/Plugin.hpp>
#include <aspl/Driver.hpp>
#include <CoreAudio/AudioServerPlugIn.h>
#include <memory>

namespace AES67 {

//
// AES67 Driver Plugin
//
// This is the main entry point for the AudioServerPlugIn
// macOS Core Audio will load this plugin and create our virtual audio device
//
class AES67Plugin : public aspl::Plugin {
public:
    explicit AES67Plugin(std::shared_ptr<aspl::Context> context)
        : aspl::Plugin(context)
    {
        AES67_LOG("AES67Plugin constructor: Creating AES67Device...");
        // Create the AES67 audio device
        device_ = std::make_shared<AES67Device>(context);
        AES67_LOG("AES67Plugin constructor: Device created successfully");

        AES67_LOG("AES67Plugin constructor: Initializing device...");
        // Initialize device (now that shared_ptr is fully constructed)
        device_->Initialize();
        AES67_LOG("AES67Plugin constructor: Device initialized successfully");

        AES67_LOG("AES67Plugin constructor: Registering device with plugin...");
        // Register device with the plugin
        AddDevice(device_);
        AES67_LOG("AES67Plugin constructor: Device registered successfully");
    }

    std::string GetManufacturer() const override {
        return "AES67 Driver Project";
    }

private:
    std::shared_ptr<AES67Device> device_;
};

} // namespace AES67

//
// C API Entry Point (Required by AudioServerPlugIn)
//

extern "C" {

// Plugin entry point called by Core Audio
void* Create() {
    AES67_LOG("=== AES67 Driver Create() called ===");

    try {
        AES67_LOG("Step 1: Creating ASPL context...");
        auto context = std::make_shared<aspl::Context>();
        AES67_LOG("Step 1: Context created successfully");

        AES67_LOG("Step 2: Creating AES67Plugin...");
        auto plugin = std::make_shared<AES67::AES67Plugin>(context);
        AES67_LOG("Step 2: Plugin created successfully");

        AES67_LOG("Step 3: Creating Driver wrapper...");
        auto driver = new aspl::Driver(context, plugin);
        AES67_LOG("Step 3: Driver created successfully");

        void* ref = driver->GetReference();
        AES67_LOGF("Step 4: Got driver reference: %p", ref);

        AES67_LOG("=== Create() completed successfully ===");
        return ref;
    }
    catch (const std::exception& e) {
        AES67_LOGF("EXCEPTION in Create(): %s", e.what());
        fprintf(stderr, "AES67 Driver: Failed to create plugin: %s\n", e.what());
        return nullptr;
    }
    catch (...) {
        AES67_LOG("UNKNOWN EXCEPTION in Create()");
        fprintf(stderr, "AES67 Driver: Unknown error during plugin creation\n");
        return nullptr;
    }
}

} // extern "C"

//
// Plugin Factory (Alternative modern C++ API)
//

namespace AES67 {

std::shared_ptr<aspl::Driver> CreateDriver() {
    auto context = std::make_shared<aspl::Context>();
    auto plugin = std::make_shared<AES67Plugin>(context);
    return std::make_shared<aspl::Driver>(context, plugin);
}

} // namespace AES67
