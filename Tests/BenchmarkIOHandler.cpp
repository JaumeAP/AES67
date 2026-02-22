//
// BenchmarkIOHandler.cpp
// AES67 macOS Driver - Build #6
// Performance benchmark for I/O handler batch processing
//

#include "../Driver/AES67IOHandler.h"
#include "../NetworkEngine/RTSafeStreamInterface.h"
#include "../Shared/RingBuffer.hpp"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <vector>
#include <numeric>
#include <cmath>
#include <utility>

using namespace AES67;
using namespace std::chrono;

struct BenchmarkResult {
    std::string testName;
    double avgTimeUs;
    double minTimeUs;
    double maxTimeUs;
    double stdDevUs;
    double throughputMBps;
    size_t iterations;
};

// Helper to create initialized ring buffer array
namespace {
    template<size_t... Is>
    auto MakeRingBufferArray(size_t bufferSize, std::index_sequence<Is...>) {
        return std::array<SPSCRingBuffer<float>, sizeof...(Is)>{
            ((void)Is, SPSCRingBuffer<float>(bufferSize))...
        };
    }

    template<size_t N>
    auto MakeRingBufferArray(size_t bufferSize) {
        return MakeRingBufferArray(bufferSize, std::make_index_sequence<N>{});
    }
}

class IOHandlerBenchmark {
public:
    IOHandlerBenchmark()
        : inputBuffers_(MakeRingBufferArray<kNumChannels>(512))
        , outputBuffers_(MakeRingBufferArray<kNumChannels>(512))
        , inputUnderruns_(0)
        , outputUnderruns_(0)
        , ioRunning_(false)
        , rtInterface_(inputBuffers_, outputBuffers_, inputUnderruns_, outputUnderruns_, ioRunning_)
    {
        // Create I/O handler using RT-safe interface
        ioHandler_ = std::make_unique<AES67IOHandler>(rtInterface_);
    }

    BenchmarkResult benchmarkInputProcessing(UInt32 frameCount, size_t iterations) {
        std::vector<double> timings;
        timings.reserve(iterations);

        // Prepare test data
        std::vector<float> outputBuffer(frameCount * kNumChannels);

        // Pre-fill input buffers with test data
        for (size_t ch = 0; ch < kNumChannels; ++ch) {
            std::vector<float> testData(frameCount);
            for (size_t i = 0; i < frameCount; ++i) {
                testData[i] = static_cast<float>(ch * 1000 + i);
            }
            inputBuffers_[ch].write(testData.data(), frameCount);
        }

        // Warmup
        for (int i = 0; i < 10; ++i) {
            ioHandler_->processInput(outputBuffer.data(), frameCount, kNumChannels);
            // Refill buffers
            for (size_t ch = 0; ch < kNumChannels; ++ch) {
                std::vector<float> testData(frameCount);
                inputBuffers_[ch].write(testData.data(), frameCount);
            }
        }

        // Benchmark
        for (size_t i = 0; i < iterations; ++i) {
            auto start = high_resolution_clock::now();

            ioHandler_->processInput(outputBuffer.data(), frameCount, kNumChannels);

            auto end = high_resolution_clock::now();
            auto duration = duration_cast<nanoseconds>(end - start);
            timings.push_back(duration.count() / 1000.0); // Convert to microseconds

            // Refill buffers for next iteration
            for (size_t ch = 0; ch < kNumChannels; ++ch) {
                std::vector<float> testData(frameCount);
                for (size_t j = 0; j < frameCount; ++j) {
                    testData[j] = static_cast<float>(ch * 1000 + j);
                }
                inputBuffers_[ch].write(testData.data(), frameCount);
            }
        }

        return calculateStatistics("Input Processing", timings, frameCount);
    }

    BenchmarkResult benchmarkOutputProcessing(UInt32 frameCount, size_t iterations) {
        std::vector<double> timings;
        timings.reserve(iterations);

        // Prepare test data
        std::vector<float> inputBuffer(frameCount * kNumChannels);
        for (size_t i = 0; i < inputBuffer.size(); ++i) {
            inputBuffer[i] = static_cast<float>(i);
        }

        // Warmup
        for (int i = 0; i < 10; ++i) {
            ioHandler_->processOutput(inputBuffer.data(), frameCount, kNumChannels);
            // Clear buffers
            for (size_t ch = 0; ch < kNumChannels; ++ch) {
                outputBuffers_[ch].reset();
            }
        }

        // Benchmark
        for (size_t i = 0; i < iterations; ++i) {
            auto start = high_resolution_clock::now();

            ioHandler_->processOutput(inputBuffer.data(), frameCount, kNumChannels);

            auto end = high_resolution_clock::now();
            auto duration = duration_cast<nanoseconds>(end - start);
            timings.push_back(duration.count() / 1000.0);

            // Clear buffers for next iteration
            for (size_t ch = 0; ch < kNumChannels; ++ch) {
                outputBuffers_[ch].reset();
            }
        }

        return calculateStatistics("Output Processing", timings, frameCount);
    }

    void printResult(const BenchmarkResult& result) {
        std::cout << std::fixed << std::setprecision(2);
        std::cout << result.testName << " (" << result.iterations << " iterations):" << std::endl;
        std::cout << "  Average: " << std::setw(8) << result.avgTimeUs << " μs" << std::endl;
        std::cout << "  Min:     " << std::setw(8) << result.minTimeUs << " μs" << std::endl;
        std::cout << "  Max:     " << std::setw(8) << result.maxTimeUs << " μs" << std::endl;
        std::cout << "  StdDev:  " << std::setw(8) << result.stdDevUs << " μs" << std::endl;
        std::cout << "  Throughput: " << std::setw(6) << result.throughputMBps << " MB/s" << std::endl;
        std::cout << std::endl;
    }

    void printEstimatedCPU(const BenchmarkResult& result, double sampleRate, UInt32 bufferSize) {
        // Calculate callback frequency
        double callbackFreqHz = sampleRate / bufferSize;

        // CPU time per callback
        double cpuTimePerCallbackMs = result.avgTimeUs / 1000.0;

        // Total CPU time per second
        double cpuTimePerSecondMs = cpuTimePerCallbackMs * callbackFreqHz;

        // CPU usage percentage (assumes single core)
        double cpuPercentage = (cpuTimePerSecondMs / 1000.0) * 100.0;

        std::cout << std::fixed << std::setprecision(2);
        std::cout << "Estimated CPU Usage (@" << sampleRate/1000 << "kHz, " << bufferSize << " frames):" << std::endl;
        std::cout << "  Callback frequency: " << callbackFreqHz << " Hz" << std::endl;
        std::cout << "  CPU time/callback:  " << cpuTimePerCallbackMs << " ms" << std::endl;
        std::cout << "  CPU usage:          " << cpuPercentage << "%" << std::endl;
        std::cout << std::endl;
    }

private:
    static constexpr size_t kNumChannels = 128;

    BenchmarkResult calculateStatistics(const std::string& name,
                                       const std::vector<double>& timings,
                                       UInt32 frameCount) {
        BenchmarkResult result;
        result.testName = name;
        result.iterations = timings.size();

        // Calculate average
        double sum = std::accumulate(timings.begin(), timings.end(), 0.0);
        result.avgTimeUs = sum / timings.size();

        // Find min/max
        result.minTimeUs = *std::min_element(timings.begin(), timings.end());
        result.maxTimeUs = *std::max_element(timings.begin(), timings.end());

        // Calculate standard deviation
        double sqSum = 0.0;
        for (double time : timings) {
            sqSum += (time - result.avgTimeUs) * (time - result.avgTimeUs);
        }
        result.stdDevUs = std::sqrt(sqSum / timings.size());

        // Calculate throughput (MB/s)
        // Data size = frameCount × kNumChannels × sizeof(float)
        double bytesPerIteration = frameCount * kNumChannels * sizeof(float);
        double bytesPerSecond = bytesPerIteration / (result.avgTimeUs / 1000000.0);
        result.throughputMBps = bytesPerSecond / (1024 * 1024);

        return result;
    }

    using DeviceChannelBuffers = std::array<SPSCRingBuffer<float>, kNumChannels>;

    DeviceChannelBuffers inputBuffers_;
    DeviceChannelBuffers outputBuffers_;
    std::atomic<uint64_t> inputUnderruns_;
    std::atomic<uint64_t> outputUnderruns_;
    std::atomic<bool> ioRunning_;
    RTSafeStreamInterface rtInterface_;
    std::unique_ptr<AES67IOHandler> ioHandler_;
};

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "AES67 I/O Handler Performance Benchmark" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;

    IOHandlerBenchmark benchmark;

    // Test different buffer sizes (common in audio)
    std::vector<UInt32> bufferSizes = {16, 32, 48, 64, 128, 256, 512};
    constexpr size_t kIterations = 10000;

    std::cout << "Testing Input Processing (Network → Core Audio)" << std::endl;
    std::cout << "================================================" << std::endl;
    std::cout << std::endl;

    for (UInt32 bufferSize : bufferSizes) {
        auto result = benchmark.benchmarkInputProcessing(bufferSize, kIterations);
        std::cout << "Buffer Size: " << bufferSize << " frames" << std::endl;
        benchmark.printResult(result);
        benchmark.printEstimatedCPU(result, 48000.0, bufferSize);
    }

    std::cout << std::endl;
    std::cout << "Testing Output Processing (Core Audio → Network)" << std::endl;
    std::cout << "=================================================" << std::endl;
    std::cout << std::endl;

    for (UInt32 bufferSize : bufferSizes) {
        auto result = benchmark.benchmarkOutputProcessing(bufferSize, kIterations);
        std::cout << "Buffer Size: " << bufferSize << " frames" << std::endl;
        benchmark.printResult(result);
        benchmark.printEstimatedCPU(result, 48000.0, bufferSize);
    }

    std::cout << "========================================" << std::endl;
    std::cout << "Benchmark Complete" << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}
