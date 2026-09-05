//
// TestDolbyModelCatalog.cpp
// AES67 macOS Driver
// Pins the Dolby model catalog: channel counts and directions per model,
// direction filtering, and the channel-sum resolver that will drive the
// driver's I/O layout from the found-elements list. Header-only, no linkage.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/DolbyModelCatalog.h"

#include <iostream>

using namespace AES67;


TEST_CASE("Channel Counts And Directions") {
    std::cout << "Test: each model has its documented channel count and side... ";
    auto* dac = DolbyModelCatalog::byId("dac3202");
    CHECK((dac && dac->channels == 32 && dac->direction == DolbyIoDirection::Output));
    auto* dma32 = DolbyModelCatalog::byId("dma32");
    CHECK((dma32 && dma32->channels == 32 && dma32->direction == DolbyIoDirection::Output));
    auto* dma16 = DolbyModelCatalog::byId("dma16");
    CHECK((dma16 && dma16->channels == 16));
    auto* dma24 = DolbyModelCatalog::byId("dma24");
    CHECK((dma24 && dma24->channels == 24));
    auto* cp850 = DolbyModelCatalog::byId("cp850");
    CHECK((cp850 && cp850->channels == 64 && cp850->direction == DolbyIoDirection::Input));
    auto* cp950 = DolbyModelCatalog::byId("cp950");
    CHECK((cp950 && cp950->channels == 16 && cp950->direction == DolbyIoDirection::Input));
    auto* cp950a = DolbyModelCatalog::byId("cp950a");
    CHECK((cp950a && cp950a->channels == 64));

    CHECK(DolbyModelCatalog::byId("nope") == nullptr);
    std::cout << "PASS" << std::endl;
}

TEST_CASE("Direction Filtering") {
    std::cout << "Test: forDirection returns only that side's models... ";
    auto outs = DolbyModelCatalog::forDirection(DolbyIoDirection::Output);
    for (const auto& m : outs) {
        CHECK(m.direction == DolbyIoDirection::Output);
    }
    // DAC3202 + DMA16/24/32 = four output models.
    CHECK(outs.size() == 4);
    auto ins = DolbyModelCatalog::forDirection(DolbyIoDirection::Input);
    CHECK(ins.size() == 3);
    std::cout << "PASS" << std::endl;
}

TEST_CASE("Total Channels Resolver") {
    std::cout << "Test: totalChannels sums assigned units per side... ";
    // A chain of three DMA32 = 96 output channels; nothing on input.
    std::vector<std::string> outputs = {"dma32", "dma32", "dma32"};
    CHECK(DolbyModelCatalog::totalChannels(outputs, DolbyIoDirection::Output) == 96);
    CHECK(DolbyModelCatalog::totalChannels(outputs, DolbyIoDirection::Input) == 0);

    // Mixed chain: DMA32 + DMA16 = 48.
    std::vector<std::string> mixed = {"dma32", "dma16"};
    CHECK(DolbyModelCatalog::totalChannels(mixed, DolbyIoDirection::Output) == 48);

    // A CP850 source = 64 input; an unknown id contributes 0, not a crash.
    std::vector<std::string> inputs = {"cp850", "mystery"};
    CHECK(DolbyModelCatalog::totalChannels(inputs, DolbyIoDirection::Input) == 64);
    std::cout << "PASS" << std::endl;
}

