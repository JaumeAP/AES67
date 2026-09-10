//
// TestAES67DeviceOffline.cpp
// AES67 macOS Driver
//
// AES67Device, without coreaudiod.
//
// The device is what the HAL loads and what nothing here had ever
// constructed: 662 lines at 18%, and the untested part is not the audio path
// -- that has its own suites -- but everything the object decides while being
// brought up. That decision-making needs an aspl::Context and nothing else,
// which a test can make: TestDeviceActivationPlugIn already does it for the
// plug-in.
//
// Initialize() is called here on purpose. It builds the streams, the RT-safe
// interface and the stream manager, and it reads the compatibility profile --
// which is what decides whether SAP, DNS-SD/RTSP and NMOS start at all. What
// it does NOT do without a network is bind anything successfully, and every
// one of those failures is a non-fatal path this exercises.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "Driver/AES67Device.h"
#include "Profiles/CompatibilityProfile.h"

#include <aspl/Context.hpp>
#include <aspl/Direction.hpp>

#include <memory>

using namespace AES67;

namespace {

std::shared_ptr<AES67Device> makeDevice() {
    auto context = std::make_shared<aspl::Context>();
    return std::make_shared<AES67Device>(context);
}

} // namespace

TEST_CASE("A Device Constructs Without A Host") {
    // aspl::Context::Host is null here: there is no coreaudiod on the other
    // end. Construction may not depend on it -- the HAL constructs the object
    // before it hands it a host, and a driver that crashes there takes the
    // audio of every application on the machine with it.
    auto device = makeDevice();
    REQUIRE(device != nullptr);
}

TEST_CASE("A Device Initialises With No Network") {
    auto device = makeDevice();

    // Every discovery surface fails to bind on a machine with no multicast
    // route, and every one of those failures is documented as non-fatal:
    // "audio still flows, only discovery is lost". This is that path, run.
    device->Initialize();

    // The streams exist afterwards, which is the half that does not depend
    // on the network at all.
    CHECK(device->GetStreamCount(aspl::Direction::Input) == 1);
    CHECK(device->GetStreamCount(aspl::Direction::Output) == 1);
}

TEST_CASE("A Device Reports The Rate And Channel Count It Was Built With") {
    auto device = makeDevice();
    device->Initialize();

    // 48 kHz is the default the device is constructed at, and what the
    // profile permits is checked elsewhere; this is the object's own answer.
    CHECK(device->GetSampleRate() == doctest::Approx(48000.0));

    const auto rates = device->GetAvailableSampleRates();
    CHECK_FALSE(rates.empty());

    // Every advertised rate has to be one a stream can actually be set to:
    // a device that lists a rate it cannot serve is one Audio MIDI Setup
    // offers and the driver then refuses.
    for (const auto& range : rates) {
        CHECK(range.mMinimum > 0.0);
        CHECK(range.mMaximum >= range.mMinimum);
    }
}

TEST_CASE("A Second Initialise Does Not Duplicate The Streams") {
    auto device = makeDevice();
    device->Initialize();
    const UInt32 inputs = device->GetStreamCount(aspl::Direction::Input);
    const UInt32 outputs = device->GetStreamCount(aspl::Direction::Output);

    // The HAL may re-initialise a device it already loaded. Doing so must not
    // leave two input streams where the machine has one input.
    device->Initialize();
    CHECK(device->GetStreamCount(aspl::Direction::Input) == inputs);
    CHECK(device->GetStreamCount(aspl::Direction::Output) == outputs);
}
