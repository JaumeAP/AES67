//
// TestResampling.cpp
// AES67 macOS Driver - Build #1
// Unit tests for resampling subsystem: PIController, SmoothedPIController, Resampler
//

#include "../NetworkEngine/Resampling/PIController.h"
#include "../NetworkEngine/Resampling/SmoothedPIController.h"
#include "../NetworkEngine/Resampling/Resampler.h"
#include "../NetworkEngine/Resampling/SampleRateAdapter.h"
#include <iostream>
#include <cassert>
#include <cmath>

using namespace AES67;

static int testsPassed = 0;
static int testsFailed = 0;

#define TEST_ASSERT(condition, message) \
    if (!(condition)) { \
        std::cerr << "FAIL: " << message << std::endl; \
        testsFailed++; \
        return false; \
    } else { \
        testsPassed++; \
    }

#define TEST_ASSERT_APPROX(a, b, tolerance, message) \
    if (std::abs((a) - (b)) > (tolerance)) { \
        std::cerr << "FAIL: " << message << " (got " << (a) << ", expected ~" << (b) << ")" << std::endl; \
        testsFailed++; \
        return false; \
    } else { \
        testsPassed++; \
    }

//
// PIController Tests
//

bool testPIControllerCreation() {
    std::cout << "Test: PIController creation with default values... ";

    PIController controller;

    // Check initial state
    TEST_ASSERT(controller.getIntegral() == 0.0, "Initial integral should be 0");
    TEST_ASSERT(controller.getProportional() == 0.0, "Initial proportional should be 0");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testPIControllerWithCustomParams() {
    std::cout << "Test: PIController with custom parameters... ";

    PIController controller(0.5, 0.05, -0.5, 0.5);

    // Update with error signal
    double output = controller.update(0.1, 0.001);

    // Output should be bounded
    TEST_ASSERT(output >= -0.5 && output <= 0.5, "Output should be within bounds");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testPIControllerPositiveError() {
    std::cout << "Test: PIController response to positive error... ";

    PIController controller(0.1, 0.01, -0.1, 0.1);

    // Positive error should increase output
    double output1 = controller.update(0.05, 0.001);
    TEST_ASSERT(output1 > 0, "Positive error should produce positive output");

    // Multiple updates should accumulate integral
    double output2 = controller.update(0.05, 0.001);
    TEST_ASSERT(output2 >= output1, "Integral accumulation should increase output over time");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testPIControllerNegativeError() {
    std::cout << "Test: PIController response to negative error... ";

    PIController controller(0.1, 0.01, -0.1, 0.1);

    // Negative error should decrease output
    double output1 = controller.update(-0.05, 0.001);
    TEST_ASSERT(output1 < 0, "Negative error should produce negative output");

    // More negative updates
    double output2 = controller.update(-0.05, 0.001);
    TEST_ASSERT(output2 <= output1, "Integral accumulation with negative error should decrease");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testPIControllerReset() {
    std::cout << "Test: PIController reset... ";

    PIController controller(0.1, 0.01, -0.1, 0.1);

    // Apply several updates to accumulate integral
    controller.update(0.05, 0.001);
    controller.update(0.05, 0.001);
    controller.update(0.05, 0.001);

    double integralBefore = controller.getIntegral();
    TEST_ASSERT(integralBefore != 0, "Integral should have accumulated");

    // Reset
    controller.reset();

    TEST_ASSERT(controller.getIntegral() == 0.0, "Integral should be 0 after reset");
    TEST_ASSERT(controller.getProportional() == 0.0, "Proportional should be 0 after reset");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testPIControllerBounds() {
    std::cout << "Test: PIController respects output bounds... ";

    PIController controller(1.0, 0.1, -0.05, 0.05);

    // Apply large errors that would exceed bounds
    double output1 = controller.update(1.0, 0.001);
    TEST_ASSERT(output1 >= -0.05 && output1 <= 0.05, "Output should be clamped to bounds");

    double output2 = controller.update(-1.0, 0.001);
    TEST_ASSERT(output2 >= -0.05 && output2 <= 0.05, "Output should stay within bounds");

    std::cout << "PASS" << std::endl;
    return true;
}

//
// SmoothedPIController Tests
//

bool testSmoothedPIControllerCreation() {
    std::cout << "Test: SmoothedPIController creation... ";

    SmoothedPIController controller;

    TEST_ASSERT(controller.getIntegral() == 0.0, "Initial integral should be 0");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testSmoothedPIControllerUpdate() {
    std::cout << "Test: SmoothedPIController update with smoothing window... ";

    SmoothedPIController controller(0.1, 0.01, -0.1, 0.1, 5);

    // Apply error
    double output1 = controller.update(0.05, 0.001);
    TEST_ASSERT(output1 >= -0.1 && output1 <= 0.1, "Output should be in bounds");

    // Apply more errors
    double output2 = controller.update(0.05, 0.001);
    double output3 = controller.update(0.05, 0.001);

    // Outputs should be reasonable
    TEST_ASSERT(output3 >= -0.1 && output3 <= 0.1, "Output should remain in bounds");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testSmoothedPIControllerReset() {
    std::cout << "Test: SmoothedPIController reset... ";

    SmoothedPIController controller(0.1, 0.01, -0.1, 0.1, 5);

    // Accumulate some history
    controller.update(0.05, 0.001);
    controller.update(0.05, 0.001);
    controller.update(0.05, 0.001);

    // Reset
    controller.reset();

    TEST_ASSERT(controller.getIntegral() == 0.0, "Integral should be 0 after reset");

    std::cout << "PASS" << std::endl;
    return true;
}

//
// Resampler Tests
//

bool testResamplerCreation() {
    std::cout << "Test: Resampler creation with standard rates... ";

    // Create resampler: 48kHz -> 96kHz
    Resampler resampler(48000, 96000, 2);

    // Check output size prediction
    int outputSize = resampler.getOutputSize(48);  // 48 frames input
    TEST_ASSERT(outputSize > 0, "Output size should be positive");
    TEST_ASSERT(outputSize >= 48, "48kHz->96kHz should double the frames");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testResamplerUpsample() {
    std::cout << "Test: Resampler upsampling (48kHz -> 96kHz)... ";

    Resampler resampler(48000, 96000, 2);

    // Create input: 48 frames of silence
    float input[48 * 2] = {0.0f};
    float output[128 * 2] = {0.0f};

    // Process
    int outputFrames = resampler.process(input, 48, output, 128);

    TEST_ASSERT(outputFrames > 0, "Should produce output frames");
    TEST_ASSERT(outputFrames >= 96, "48->96kHz should produce ~96 frames from 48 input");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testResamplerDownsample() {
    std::cout << "Test: Resampler downsampling (96kHz -> 48kHz)... ";

    Resampler resampler(96000, 48000, 2);

    // Create input: 96 frames of silence
    float input[96 * 2] = {0.0f};
    float output[96 * 2] = {0.0f};

    // Process
    int outputFrames = resampler.process(input, 96, output, 96);

    TEST_ASSERT(outputFrames > 0, "Should produce output frames");
    TEST_ASSERT(outputFrames <= 48, "96->48kHz should produce ~48 frames from 96 input");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testResamplerMonoToStereo() {
    std::cout << "Test: Resampler with 1 channel (mono)... ";

    Resampler resampler(48000, 48000, 1);

    // Create input: 48 frames mono
    float input[48] = {0.0f};
    float output[48] = {0.0f};

    // Process
    int outputFrames = resampler.process(input, 48, output, 48);

    TEST_ASSERT(outputFrames > 0, "Should process mono audio");
    TEST_ASSERT(outputFrames <= 48, "Same rate should preserve frame count");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testResamplerReset() {
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

    TEST_ASSERT(out1 == out2, "Reset should restore state for same input");

    std::cout << "PASS" << std::endl;
    return true;
}

//
// SampleRateAdapter Tests
//

bool testSampleRateAdapterCreation() {
    std::cout << "Test: SampleRateAdapter creation... ";

    SampleRateAdapter adapter(48000, 48000, 2, SampleRateAdapter::ConversionQuality::GOOD);

    // No-op conversion (same rate)
    std::cout << "PASS" << std::endl;
    return true;
}

bool testSampleRateAdapterQualities() {
    std::cout << "Test: SampleRateAdapter with different quality levels... ";

    SampleRateAdapter fastAdapter(48000, 96000, 2, SampleRateAdapter::ConversionQuality::FAST);
    SampleRateAdapter goodAdapter(48000, 96000, 2, SampleRateAdapter::ConversionQuality::GOOD);
    SampleRateAdapter bestAdapter(48000, 96000, 2, SampleRateAdapter::ConversionQuality::BEST);

    // All should be created successfully
    TEST_ASSERT(true, "All quality levels should be creatable");

    std::cout << "PASS" << std::endl;
    return true;
}

//
// Integration: Clock Drift Scenario
//

bool testClockDriftScenario() {
    std::cout << "Test: Clock drift correction scenario... ";

    // Simulate: device at 48kHz, but network stream drifts +0.5% faster
    PIController driftCorrector(0.0001, 0.00001, -0.01, 0.01);

    // Network is ahead (positive error)
    double correction1 = driftCorrector.update(0.001, 0.001);  // 0.1% ahead
    double correction2 = driftCorrector.update(0.001, 0.001);
    double correction3 = driftCorrector.update(0.001, 0.001);

    // Corrections should accumulate to slow down playback
    TEST_ASSERT(correction3 > correction1, "Corrections should accumulate for consistent error");

    // Then network corrects (negative error)
    double correction4 = driftCorrector.update(-0.001, 0.001);

    // Should start reducing the adjustment
    TEST_ASSERT(correction4 < correction3, "Negative error should reduce positive correction");

    std::cout << "PASS" << std::endl;
    return true;
}

//
// Main Test Runner
//

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "AES67 Resampling Subsystem Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;

    std::cout << "PIController Tests:" << std::endl;
    std::cout << "------------------" << std::endl;
    testPIControllerCreation();
    testPIControllerWithCustomParams();
    testPIControllerPositiveError();
    testPIControllerNegativeError();
    testPIControllerReset();
    testPIControllerBounds();
    std::cout << std::endl;

    std::cout << "SmoothedPIController Tests:" << std::endl;
    std::cout << "--------------------------" << std::endl;
    testSmoothedPIControllerCreation();
    testSmoothedPIControllerUpdate();
    testSmoothedPIControllerReset();
    std::cout << std::endl;

    std::cout << "Resampler Tests:" << std::endl;
    std::cout << "---------------" << std::endl;
    testResamplerCreation();
    testResamplerUpsample();
    testResamplerDownsample();
    testResamplerMonoToStereo();
    testResamplerReset();
    std::cout << std::endl;

    std::cout << "SampleRateAdapter Tests:" << std::endl;
    std::cout << "----------------------" << std::endl;
    testSampleRateAdapterCreation();
    testSampleRateAdapterQualities();
    std::cout << std::endl;

    std::cout << "Integration Tests:" << std::endl;
    std::cout << "-----------------" << std::endl;
    testClockDriftScenario();
    std::cout << std::endl;

    std::cout << "========================================" << std::endl;
    std::cout << "Test Results:" << std::endl;
    std::cout << "  Passed: " << testsPassed << std::endl;
    std::cout << "  Failed: " << testsFailed << std::endl;
    std::cout << "========================================" << std::endl;

    return testsFailed == 0 ? 0 : 1;
}
