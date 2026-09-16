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
#include "Shared/CustomProperties.h"

#include <aspl/Context.hpp>
#include <aspl/Direction.hpp>

#include <CoreFoundation/CoreFoundation.h>

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

TEST_CASE("A Rate The Device Does Not Support Is Refused") {
    auto device = makeDevice();
    device->Initialize();

    const Float64 before = device->GetSampleRate();

    // 47999 Hz is within a hair of 48000 and still not one of the eight:
    // the check is a 0.1 Hz tolerance around each advertised rate, not a
    // nearest-match, because serving a rate the streams are not formatted
    // for is worse than refusing it.
    CHECK(device->SetSampleRate(47999.0) == kAudioHardwareUnsupportedOperationError);
    CHECK(device->SetSampleRate(0.0) == kAudioHardwareUnsupportedOperationError);
    CHECK(device->GetSampleRate() == doctest::Approx(before));
}

TEST_CASE("Every Advertised Rate Can Actually Be Set") {
    auto device = makeDevice();
    device->Initialize();

    // The complement of the existing check that the ranges are well formed:
    // a rate this device offers Audio MIDI Setup has to be one it then
    // accepts, or the user picks it and the driver says no.
    for (const auto& range : device->GetAvailableSampleRates()) {
        CHECK(device->SetSampleRate(range.mMinimum) == kAudioHardwareNoError);
        CHECK(device->GetSampleRate() == doctest::Approx(range.mMinimum));
    }
}

TEST_CASE("The Rate Cannot Change While IO Is Running") {
    auto device = makeDevice();
    device->Initialize();

    REQUIRE(device->SetSampleRate(48000.0) == kAudioHardwareNoError);

    // startCount == 0 is the first client: this is the transition that turns
    // ioRunning_ on and starts the RTP threads.
    REQUIRE(device->StartIOImpl(1, 0) == kAudioHardwareNoError);

    // Changing the rate under a running client would reformat the streams
    // the client is already reading, so it is refused rather than applied.
    CHECK(device->SetSampleRate(96000.0) == kAudioHardwareBadObjectError);
    CHECK(device->GetSampleRate() == doctest::Approx(48000.0));

    REQUIRE(device->StopIOImpl(1, 0) == kAudioHardwareNoError);

    // And allowed again once the last client has gone.
    CHECK(device->SetSampleRate(96000.0) == kAudioHardwareNoError);
}

TEST_CASE("Starting And Stopping IO Follows The Client Count") {
    auto device = makeDevice();
    device->Initialize();

    auto input = device->GetInputStream();
    auto output = device->GetOutputStream();
    REQUIRE(input != nullptr);
    REQUIRE(output != nullptr);

    REQUIRE(device->StartIOImpl(1, 0) == kAudioHardwareNoError);
    CHECK(input->GetIsActive());
    CHECK(output->GetIsActive());

    // A second client arriving is startCount == 1, not 0: the device is
    // already running and must not repeat the transition.
    REQUIRE(device->StartIOImpl(2, 1) == kAudioHardwareNoError);
    CHECK(input->GetIsActive());

    // The first of two leaving is startCount == 1 as well, and the streams
    // stay active because a client is still there.
    REQUIRE(device->StopIOImpl(1, 1) == kAudioHardwareNoError);
    CHECK(input->GetIsActive());

    // Only the last one out deactivates them.
    REQUIRE(device->StopIOImpl(2, 0) == kAudioHardwareNoError);
    CHECK_FALSE(input->GetIsActive());
    CHECK_FALSE(output->GetIsActive());
}

TEST_CASE("Only An Advertised Buffer Size Is Accepted") {
    auto device = makeDevice();
    device->Initialize();

    const auto sizes = device->GetAvailableBufferSizes();
    REQUIRE_FALSE(sizes.empty());

    for (UInt32 size : sizes) {
        CHECK(device->SetBufferSize(size) == kAudioHardwareNoError);
        CHECK(device->GetBufferSize() == size);
    }

    // 63 sits between two advertised sizes and is not one of them. The list
    // is an exact-match list, so it is refused and the previous size stands.
    const UInt32 kept = device->GetBufferSize();
    CHECK(device->SetBufferSize(63) == kAudioHardwareUnsupportedOperationError);
    CHECK(device->SetBufferSize(0) == kAudioHardwareUnsupportedOperationError);
    CHECK(device->GetBufferSize() == kept);
}

TEST_CASE("The Device Identifies Itself The Way It Was Constructed") {
    auto device = makeDevice();

    // These three are what a client matches on to find this driver among
    // every other audio device, and DriverManager.swift looks the UID up by
    // the literal string: changing one of them silently unpairs the app
    // from the driver.
    CHECK(device->GetDeviceName() == "AES67 Device");
    CHECK(device->GetDeviceManufacturer() == "AES67 Driver");
    CHECK(device->GetDeviceUID() == "com.aes67.driver.device");

    // And they have to agree with what was handed to aspl::DeviceParameters,
    // which is what the HAL publishes -- two places, one answer.
    CHECK(device->GetName() == device->GetDeviceName());
    CHECK(device->GetManufacturer() == device->GetDeviceManufacturer());
}

TEST_CASE("Resetting The Statistics Zeroes Both Counters") {
    auto device = makeDevice();
    device->Initialize();

    // Nothing has run, so they are zero already; what this pins is that the
    // call exists, touches both directions, and leaves them readable.
    device->ResetStatistics();
    CHECK(device->GetInputUnderrunCount() == 0);
    CHECK(device->GetOutputUnderrunCount() == 0);
}

//
// The gateway: the four custom AudioObject properties ManagerApp reads. They
// are private getters registered in the constructor, so the way in is the
// same one the HAL uses -- GetPropertyData() with the custom selector, which
// falls through aspl::Object's dispatch into the registered getter.
//
namespace {

/// Queries one custom property and hands back the +1 reference its getter
/// returned, or null with the status in `status`.
CFPropertyListRef queryCustomProperty(AES67Device& device,
                                      AudioObjectPropertySelector selector,
                                      OSStatus& status) {
    const AudioObjectPropertyAddress address{
        selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};

    CFPropertyListRef value = nullptr;
    UInt32 returnedSize = 0;
    status = device.GetPropertyData(device.GetID(), 0, &address,
                                    0, nullptr, sizeof(value), &returnedSize, &value);
    return value;
}

} // namespace

TEST_CASE("The PTP Diagnostics Property Answers Before Anything Is Connected") {
    auto device = makeDevice();
    device->Initialize();

    OSStatus status = kAudioHardwareUnspecifiedError;
    CFPropertyListRef value = queryCustomProperty(
        *device, kPTPDiagnosticsPropertySelector, status);

    REQUIRE(status == kAudioHardwareNoError);
    REQUIRE(value != nullptr);
    REQUIRE(CFGetTypeID(value) == CFDictionaryGetTypeID());

    auto dict = static_cast<CFDictionaryRef>(value);

    // Every key the Swift side reads has to be present even when there is no
    // grandmaster: a missing key there reads as zero and shows as "locked".
    for (const char* key : {kPTPDiagKeyIsConnected, kPTPDiagKeyIsLocked,
                            kPTPDiagKeyMasterClockID, kPTPDiagKeyClockClass,
                            kPTPDiagKeyClockAccuracy, kPTPDiagKeyOffsetNs,
                            kPTPDiagKeyCurrentDomain, kPTPDiagKeyRole,
                            kPTPDiagKeyEverWasMaster, kPTPDiagKeyHasCompetitor,
                            kPTPDiagKeyCompetitorPriority1, kPTPDiagKeyCompetitorPriority2,
                            kPTPDiagKeySyncMessagesReceived,
                            kPTPDiagKeyAnnounceMessagesReceived}) {
        CFStringRef keyRef = CFStringCreateWithCString(
            kCFAllocatorDefault, key, kCFStringEncodingUTF8);
        REQUIRE(keyRef != nullptr);
        INFO("key: " << key);
        CHECK(CFDictionaryContainsKey(dict, keyRef));
        CFRelease(keyRef);
    }

    CFRelease(value);
}

TEST_CASE("The Diagnostics Property Answers Before Initialize Too") {
    auto device = makeDevice();

    // Registered in the constructor, queried before streamManager_ exists:
    // the getter's own null guard is what keeps the HAL from reading a
    // half-built device, and this is that path.
    OSStatus status = kAudioHardwareUnspecifiedError;
    CFPropertyListRef value = queryCustomProperty(
        *device, kPTPDiagnosticsPropertySelector, status);

    REQUIRE(status == kAudioHardwareNoError);
    REQUIRE(value != nullptr);
    CHECK(CFGetTypeID(value) == CFDictionaryGetTypeID());
    CFRelease(value);
}

TEST_CASE("The Three List Properties Are Empty Arrays With Nothing On The Network") {
    auto device = makeDevice();
    device->Initialize();

    // Sessions, PTP peers and RTCP reporters: nothing has been heard on a
    // machine with no multicast route, and each has to say so as an empty
    // array rather than as a null the app would have to special-case.
    for (AudioObjectPropertySelector selector : {kDiscoveredSessionsPropertySelector,
                                                 kPtpPeersPropertySelector,
                                                 kRtcpReceiversPropertySelector}) {
        OSStatus status = kAudioHardwareUnspecifiedError;
        CFPropertyListRef value = queryCustomProperty(*device, selector, status);

        REQUIRE(status == kAudioHardwareNoError);
        REQUIRE(value != nullptr);
        REQUIRE(CFGetTypeID(value) == CFArrayGetTypeID());
        CHECK(CFArrayGetCount(static_cast<CFArrayRef>(value)) == 0);
        CFRelease(value);
    }
}

TEST_CASE("A Selector Nobody Registered Is Not Answered") {
    auto device = makeDevice();
    device->Initialize();

    // The complement of the four above: the dispatch has to reject a
    // selector it does not know rather than return a stale one.
    OSStatus status = kAudioHardwareNoError;
    CFPropertyListRef value = queryCustomProperty(*device, 0x61363700, status);

    CHECK(status == kAudioHardwareUnknownPropertyError);
    CHECK(value == nullptr);
}
