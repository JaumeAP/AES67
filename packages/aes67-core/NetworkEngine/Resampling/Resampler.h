#ifndef RESAMPLER_H
#define RESAMPLER_H

#include <vector>
#include <cstddef>
#include <cstdint>

namespace AES67 {

class Resampler {
public:
    Resampler(double inputSampleRate, double outputSampleRate, int channels = 2);
    ~Resampler();
    
    // Process audio samples - returns number of output samples
    int process(const float* input, int inputFrames, float* output, int outputFrames, bool endOfInput = false);
    
    // Get the number of output frames needed for a given number of input frames
    int getOutputSize(int inputFrames) const;
    
    // Reset the resampler state
    void reset();
    
private:
    void* resamplerState_;  // Opaque pointer to underlying resampler implementation
};

} // namespace AES67

#endif // RESAMPLER_H