//
// PlugInMain.cpp
// AES67 macOS Driver - Build #6
// AudioServerPlugIn entry point
//

#include "Driver/AES67Plugin.h"
#include "Driver/DebugLog.h"
#include <aspl/Driver.hpp>
#include <CoreAudio/AudioServerPlugIn.h>
#include <memory>


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

        // Explicit cast: GetReference() returns AudioServerPlugInDriverRef, which
        // is a pointer to a pointer, and CoreAudio's factory signature wants
        // void*. The conversion is the contract, but an implicit multilevel
        // pointer conversion is worth spelling out rather than letting it
        // happen quietly.
        void* ref = static_cast<void*>(driver->GetReference());
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
