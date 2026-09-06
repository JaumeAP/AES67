//
// AES67Plugin.h
// AES67 macOS Driver
// The AudioServerPlugIn object Core Audio builds, and the one decision it
// makes while being built: whether to publish a device at all.
//
// A header rather than a class buried in PlugInMain.cpp so that the decision
// can be tested. Tests/TestDeviceActivationPlugIn.cpp constructs this with the
// activation flag set both ways and counts the devices that came out; that is
// the whole behaviour, and it is not reachable through the C entry point,
// which hands its plug-in to Core Audio and keeps nothing.
//
#pragma once

#include "AES67Device.h"
#include "Driver/DebugLog.h"
#include "Driver/DeviceActivation.h"

#include <aspl/Plugin.hpp>

#include <memory>

namespace AES67 {

class AES67Plugin : public aspl::Plugin {
public:
    explicit AES67Plugin(std::shared_ptr<aspl::Context> context)
        : aspl::Plugin(context)
    {
        // Deactivated means installed but not published: no device is built
        // and none is registered, so nothing appears in Core Audio and the
        // settings the driver only reads at startup are free to be edited.
        // See Driver/DeviceActivation.h.
        if (!DeviceActivationManager().load().active) {
            AES67_LOG("AES67Plugin constructor: device deactivated, publishing nothing");
            return;
        }

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
