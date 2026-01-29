#include "SampleRateAdapter.h"
#include <cmath>
#include <cstring>
#include <vector>

namespace AES67 {

// Simple linear interpolation resampler implementation
class LinearResampler {
public:
    LinearResampler(double inputRate, double outputRate, int channels)
        : inputRate_(inputRate), outputRate_(outputRate), channels_(channels) {
        ratio_ = outputRate_ / inputRate_;
        bufferSize_ = 4096; // Default buffer size
        buffer_ = new float[bufferSize_ * channels_];
        bufferPos_ = 0;
        lastSample_.resize(channels_);
        for (int i = 0; i < channels_; ++i) {
            lastSample_[i] = 0.0f;
        }
    }
    
    ~LinearResampler() {
        delete[] buffer_;
    }
    
    int process(const float* input, int inputFrames, float* output, int outputFrames, bool endOfInput = false) {
        // Linear interpolation resampling algorithm
        int outputIndex = 0;
        double inputIndex = 0.0;
        int inputIndexInt = 0;
        double fraction = 0.0;
        
        // Copy new input to our internal buffer
        for (int i = 0; i < inputFrames && bufferPos_ < bufferSize_; ++i) {
            for (int ch = 0; ch < channels_; ++ch) {
                buffer_[bufferPos_ * channels_ + ch] = input[i * channels_ + ch];
            }
            bufferPos_++;
        }
        
        // Perform resampling using linear interpolation
        while (outputIndex < outputFrames && inputIndex < bufferPos_) {
            inputIndexInt = static_cast<int>(inputIndex);
            fraction = inputIndex - inputIndexInt;
            
            // Linear interpolation between adjacent samples
            for (int ch = 0; ch < channels_; ++ch) {
                float sample1 = buffer_[inputIndexInt * channels_ + ch];
                float sample2 = (inputIndexInt + 1 < bufferPos_) ? 
                               buffer_[(inputIndexInt + 1) * channels_ + ch] : lastSample_[ch];
                
                output[outputIndex * channels_ + ch] = sample1 + fraction * (sample2 - sample1);
            }
            
            outputIndex++;
            inputIndex += 1.0 / ratio_;
        }
        
        // Update last sample for potential use when buffer is empty
        if (bufferPos_ > 0) {
            for (int ch = 0; ch < channels_; ++ch) {
                lastSample_[ch] = buffer_[(bufferPos_ - 1) * channels_ + ch];
            }
        }
        
        // Shift remaining samples to the beginning of the buffer
        int remaining = bufferPos_ - static_cast<int>(inputIndex);
        if (remaining > 0) {
            memmove(buffer_, 
                   buffer_ + static_cast<int>(inputIndex) * channels_, 
                   remaining * channels_ * sizeof(float));
        }
        bufferPos_ = remaining;
        
        return outputIndex;
    }
    
    int getOutputSize(int inputFrames) const {
        return static_cast<int>(inputFrames * ratio_);
    }
    
    void reset() {
        bufferPos_ = 0;
        for (int i = 0; i < channels_; ++i) {
            lastSample_[i] = 0.0f;
        }
    }

private:
    double inputRate_;
    double outputRate_;
    int channels_;
    double ratio_;
    
    // Internal buffering
    float* buffer_;
    int bufferSize_;
    int bufferPos_;
    
    // Last sample values for interpolation
    std::vector<float> lastSample_;
};

class SampleRateAdapter::Impl {
public:
    Impl(double inputRate, double outputRate, int channels, ConversionQuality quality)
        : inputRate_(inputRate), outputRate_(outputRate), channels_(channels), quality_(quality) {
        needsConversion_ = std::abs(inputRate_ - outputRate_) > 0.1; // Account for floating point precision
        
        if (needsConversion_) {
            resampler_ = std::make_unique<LinearResampler>(inputRate_, outputRate_, channels_);
        }
    }
    
    int process(const float* input, int inputFrames, float* output, int outputFrames) {
        if (!needsConversion_) {
            // Passthrough - no conversion needed
            int framesToCopy = std::min(inputFrames, outputFrames);
            std::memcpy(output, input, framesToCopy * channels_ * sizeof(float));
            return framesToCopy;
        }
        
        return resampler_->process(input, inputFrames, output, outputFrames);
    }
    
    double getRatio() const {
        return needsConversion_ ? outputRate_ / inputRate_ : 1.0;
    }
    
    void reset() {
        if (resampler_) {
            resampler_->reset();
        }
    }
    
    bool isPassthrough() const {
        return !needsConversion_;
    }

private:
    double inputRate_;
    double outputRate_;
    int channels_;
    ConversionQuality quality_;
    bool needsConversion_;
    std::unique_ptr<LinearResampler> resampler_;
};

SampleRateAdapter::SampleRateAdapter(double inputRate, double outputRate, int channels, ConversionQuality quality)
    : pimpl_(std::make_unique<Impl>(inputRate, outputRate, channels, quality)) {
}

SampleRateAdapter::~SampleRateAdapter() = default;

int SampleRateAdapter::process(const float* input, int inputFrames, float* output, int outputFrames) {
    return pimpl_->process(input, inputFrames, output, outputFrames);
}

double SampleRateAdapter::getRatio() const {
    return pimpl_->getRatio();
}

void SampleRateAdapter::reset() {
    pimpl_->reset();
}

bool SampleRateAdapter::isPassthrough() const {
    return pimpl_->isPassthrough();
}

} // namespace AES67