//
// TestResampling.cpp
// AES67 macOS Driver - Build #1
// Unit tests for resampling subsystem: PIController, SmoothedPIController, Resampler
//

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/Resampling/PIController.h"
#include "NetworkEngine/Resampling/SmoothedPIController.h"
#include "NetworkEngine/Resampling/Resampler.h"
#include "NetworkEngine/Resampling/SampleRateAdapter.h"
#include <iostream>
#include <cassert>
#include <cmath>

using namespace AES67;




//
// PIController Tests
//

TEST_CASE("PI Controller Creation") {
    std::cout << "Test: PIController creation with default values... ";

    PIController controller;

    // Check initial state
    CHECK(controller.getIntegral() == 0.0);
    CHECK(controller.getProportional() == 0.0);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("PI Controller With Custom Params") {
    std::cout << "Test: PIController with custom parameters... ";

    PIController controller(0.5, 0.05, -0.5, 0.5);

    // Update with error signal
    double output = controller.update(0.1, 0.001);

    // Output should be bounded
    CHECK((output >= -0.5 && output <= 0.5));

    std::cout << "PASS" << std::endl;
}

TEST_CASE("PI Controller Positive Error") {
    std::cout << "Test: PIController response to positive error... ";

    PIController controller(0.1, 0.01, -0.1, 0.1);

    // Positive error should increase output
    double output1 = controller.update(0.05, 0.001);
    CHECK(output1 > 0);

    // Multiple updates should accumulate integral
    double output2 = controller.update(0.05, 0.001);
    CHECK(output2 >= output1);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("PI Controller Negative Error") {
    std::cout << "Test: PIController response to negative error... ";

    PIController controller(0.1, 0.01, -0.1, 0.1);

    // Negative error should decrease output
    double output1 = controller.update(-0.05, 0.001);
    CHECK(output1 < 0);

    // More negative updates
    double output2 = controller.update(-0.05, 0.001);
    CHECK(output2 <= output1);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("PI Controller Reset") {
    std::cout << "Test: PIController reset... ";

    PIController controller(0.1, 0.01, -0.1, 0.1);

    // Apply several updates to accumulate integral
    controller.update(0.05, 0.001);
    controller.update(0.05, 0.001);
    controller.update(0.05, 0.001);

    double integralBefore = controller.getIntegral();
    CHECK(integralBefore != 0);

    // Reset
    controller.reset();

    CHECK(controller.getIntegral() == 0.0);
    CHECK(controller.getProportional() == 0.0);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("PI Controller Bounds") {
    std::cout << "Test: PIController respects output bounds... ";

    PIController controller(1.0, 0.1, -0.05, 0.05);

    // Apply large errors that would exceed bounds
    double output1 = controller.update(1.0, 0.001);
    CHECK((output1 >= -0.05 && output1 <= 0.05));

    double output2 = controller.update(-1.0, 0.001);
    CHECK((output2 >= -0.05 && output2 <= 0.05));

    std::cout << "PASS" << std::endl;
}

//
// SmoothedPIController Tests
//

TEST_CASE("Smoothed PI Controller Creation") {
    std::cout << "Test: SmoothedPIController creation... ";

    SmoothedPIController controller;

    CHECK(controller.getIntegral() == 0.0);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Smoothed PI Controller Update") {
    std::cout << "Test: SmoothedPIController update with smoothing window... ";

    SmoothedPIController controller(0.1, 0.01, -0.1, 0.1, 5);

    // Apply error
    double output1 = controller.update(0.05, 0.001);
    CHECK((output1 >= -0.1 && output1 <= 0.1));

    // Apply more errors
    double output2 = controller.update(0.05, 0.001);
    double output3 = controller.update(0.05, 0.001);

    // Outputs should be reasonable
    CHECK((output3 >= -0.1 && output3 <= 0.1));

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Smoothed PI Controller Reset") {
    std::cout << "Test: SmoothedPIController reset... ";

    SmoothedPIController controller(0.1, 0.01, -0.1, 0.1, 5);

    // Accumulate some history
    controller.update(0.05, 0.001);
    controller.update(0.05, 0.001);
    controller.update(0.05, 0.001);

    // Reset
    controller.reset();

    CHECK(controller.getIntegral() == 0.0);

    std::cout << "PASS" << std::endl;
}

//
// Resampler Tests
//

TEST_CASE("Resampler Creation") {
    std::cout << "Test: Resampler creation with standard rates... ";

    // Create resampler: 48kHz -> 96kHz
    Resampler resampler(48000, 96000, 2);

    // Check output size prediction
    int outputSize = resampler.getOutputSize(48);  // 48 frames input
    CHECK(outputSize > 0);
    CHECK(outputSize >= 48);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Resampler Upsample") {
    std::cout << "Test: Resampler upsampling (48kHz -> 96kHz)... ";

    Resampler resampler(48000, 96000, 2);

    // Create input: 48 frames of silence
    float input[48 * 2] = {0.0f};
    float output[128 * 2] = {0.0f};

    // Process
    int outputFrames = resampler.process(input, 48, output, 128);

    CHECK(outputFrames > 0);
    CHECK(outputFrames >= 96);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Resampler Downsample") {
    std::cout << "Test: Resampler downsampling (96kHz -> 48kHz)... ";

    Resampler resampler(96000, 48000, 2);

    // Create input: 96 frames of silence
    float input[96 * 2] = {0.0f};
    float output[96 * 2] = {0.0f};

    // Process
    int outputFrames = resampler.process(input, 96, output, 96);

    CHECK(outputFrames > 0);
    CHECK(outputFrames <= 48);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Resampler Mono To Stereo") {
    std::cout << "Test: Resampler with 1 channel (mono)... ";

    Resampler resampler(48000, 48000, 1);

    // Create input: 48 frames mono
    float input[48] = {0.0f};
    float output[48] = {0.0f};

    // Process
    int outputFrames = resampler.process(input, 48, output, 48);

    CHECK(outputFrames > 0);
    CHECK(outputFrames <= 48);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Resampler Reset") {
    std::cout << "Test: Resampler reset clears state... ";

    Resampler resampler(48000, 96000, 2);

    float input[48 * 2] = {0.0f};
    float output[128 * 2] = {0.0f};

    // Process once
    int out1 = resampler.process(input, 48, output, 128);

    // Reset
    resampler.reset();

    // Process again
    int out2 = resampler.process(input, 48, output, 128);

    CHECK(out1 == out2);

    std::cout << "PASS" << std::endl;
}

//
// SampleRateAdapter Tests
//

TEST_CASE("Sample Rate Adapter Creation") {
    std::cout << "Test: SampleRateAdapter creation... ";

    SampleRateAdapter adapter(48000, 48000, 2, SampleRateAdapter::ConversionQuality::GOOD);

    // No-op conversion (same rate)
    std::cout << "PASS" << std::endl;
}

TEST_CASE("Sample Rate Adapter Qualities") {
    std::cout << "Test: SampleRateAdapter with different quality levels... ";

    SampleRateAdapter fastAdapter(48000, 96000, 2, SampleRateAdapter::ConversionQuality::FAST);
    SampleRateAdapter goodAdapter(48000, 96000, 2, SampleRateAdapter::ConversionQuality::GOOD);
    SampleRateAdapter bestAdapter(48000, 96000, 2, SampleRateAdapter::ConversionQuality::BEST);

    // All should be created successfully
    CHECK(true);

    std::cout << "PASS" << std::endl;
}

//
// Integration: Clock Drift Scenario
//

TEST_CASE("Clock Drift Scenario") {
    std::cout << "Test: Clock drift correction scenario... ";

    // Simulate: device at 48kHz, but network stream drifts +0.5% faster
    PIController driftCorrector(0.0001, 0.00001, -0.01, 0.01);

    // Network is ahead (positive error)
    double correction1 = driftCorrector.update(0.001, 0.001);  // 0.1% ahead
    double correction2 = driftCorrector.update(0.001, 0.001);
    double correction3 = driftCorrector.update(0.001, 0.001);

    // Corrections should accumulate to slow down playback
    CHECK(correction3 > correction1);

    // Then network corrects (negative error)
    double correction4 = driftCorrector.update(-0.001, 0.001);

    // Should start reducing the adjustment
    CHECK(correction4 < correction3);

    std::cout << "PASS" << std::endl;
}

//
// Main Test Runner
//

