#include "Resampler.h"
#include <cstdlib>
#include <cstring>

// For this implementation, we'll use a simple linear interpolation resampler
// In a production environment, you would use a high-quality resampling library like libsamplerate

namespace AES67 {

class SimpleResampler {
public:
    SimpleResampler(double inputRate, double outputRate, int channels)
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
    
    ~SimpleResampler() {
        delete[] buffer_;
    }
    
    int process(const float* input, int inputFrames, float* output, int outputFrames, bool endOfInput) {
        // Simple linear interpolation resampling algorithm
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

Resampler::Resampler(double inputSampleRate, double outputSampleRate, int channels) {
    resamplerState_ = new SimpleResampler(inputSampleRate, outputSampleRate, channels);
}

Resampler::~Resampler() {
    if (resamplerState_) {
        delete static_cast<SimpleResampler*>(resamplerState_);
    }
}

int Resampler::process(const float* input, int inputFrames, float* output, int outputFrames, bool endOfInput) {
    SimpleResampler* simpleResampler = static_cast<SimpleResampler*>(resamplerState_);
    return simpleResampler->process(input, inputFrames, output, outputFrames, endOfInput);
}

int Resampler::getOutputSize(int inputFrames) const {
    SimpleResampler* simpleResampler = static_cast<SimpleResampler*>(resamplerState_);
    return simpleResampler->getOutputSize(inputFrames);
}

void Resampler::reset() {
    SimpleResampler* simpleResampler = static_cast<SimpleResampler*>(resamplerState_);
    simpleResampler->reset();
}

} // namespace AES67