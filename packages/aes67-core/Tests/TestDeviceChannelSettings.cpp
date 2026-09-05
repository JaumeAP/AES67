//
// TestDeviceChannelSettings.cpp
// The usable-channel cap, which had no tests.
//
// This decides how many channels the device advertises as usable on top of the
// fixed 128-channel buffers. Two things about it are easy to get wrong and both
// are pinned here: the allowed counts are every multiple of eight rather than a
// coarse preset list, and the auxiliary group has to actually fit -- at 128 it
// does not, so enabling it there is invalid rather than silently clamped.
//

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/DeviceChannelSettings.h"

#include <algorithm>

using AES67::DeviceChannelSelection;
using AES67::DeviceChannelSettings;

TEST_CASE("Allowed counts are every multiple of eight up to the maximum") {
    const auto& allowed = DeviceChannelSettings::allowedChannelCounts();

    REQUIRE(allowed.empty() == false);
    CHECK(allowed.front() == DeviceChannelSettings::kChannelGroupSize);
    CHECK(allowed.back() == DeviceChannelSettings::kMaxDeviceChannels);
    CHECK(allowed.size() == DeviceChannelSettings::kMaxDeviceChannels /
                            DeviceChannelSettings::kChannelGroupSize);

    // The point of the list: a detected layout of 48 or 96 channels is exposed
    // exactly, not rounded up to the next power of two.
    CHECK(std::find(allowed.begin(), allowed.end(), 48u) != allowed.end());
    CHECK(std::find(allowed.begin(), allowed.end(), 96u) != allowed.end());

    for (uint32_t count : allowed) {
        CHECK(count % DeviceChannelSettings::kChannelGroupSize == 0);
    }
}

TEST_CASE("A count outside the list is invalid") {
    DeviceChannelSelection sel;
    sel.auxChannelEnabled = false;

    sel.channelCount = 32;
    CHECK(sel.isValid());

    sel.channelCount = 33;   // not a multiple of eight
    CHECK(sel.isValid() == false);

    sel.channelCount = 0;
    CHECK(sel.isValid() == false);

    sel.channelCount = DeviceChannelSettings::kMaxDeviceChannels + 8;
    CHECK(sel.isValid() == false);
}

TEST_CASE("The auxiliary group has to fit, and at the maximum it does not") {
    DeviceChannelSelection sel;
    sel.auxChannelEnabled = true;

    sel.channelCount = 120;
    CHECK(sel.isValid());

    // 128 + 8 exceeds the fixed buffers, so this is refused rather than
    // quietly trimmed -- a caller asking for both gets told no.
    sel.channelCount = DeviceChannelSettings::kMaxDeviceChannels;
    CHECK(sel.isValid() == false);
}

TEST_CASE("Total channel count adds the auxiliary group and never exceeds the buffers") {
    DeviceChannelSelection sel;

    sel.channelCount = 32;
    sel.auxChannelEnabled = false;
    CHECK(sel.totalChannelCount() == 32);

    sel.auxChannelEnabled = true;
    CHECK(sel.totalChannelCount() == 32 + DeviceChannelSettings::kChannelGroupSize);

    // Saturates rather than overflowing past the fixed 128-channel buffers,
    // which is what the IO thread indexes into.
    sel.channelCount = DeviceChannelSettings::kMaxDeviceChannels;
    CHECK(sel.totalChannelCount() == DeviceChannelSettings::kMaxDeviceChannels);
}
