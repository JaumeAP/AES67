#ifndef SAMPLE_RATE_ADAPTER_H
#define SAMPLE_RATE_ADAPTER_H

#include <memory>

namespace AES67 {

/**
 * Handles sample rate conversion when network stream rate differs from device rate
 * 
 * Critical for AES67 interoperability when sample rates don't match
 */
class SampleRateAdapter {
public:
    enum class ConversionQuality {
        FAST,      // Linear interpolation - fast but lower quality
        GOOD,      // Cubic interpolation - good balance of quality and performance
        BEST       // Sinc-based - highest quality but computationally expensive
    };
    
    /**
     * Constructor
     * @param inputRate Input sample rate
     * @param outputRate Output sample rate
     * @param channels Number of channels
     * @param quality Quality of conversion
     */
    SampleRateAdapter(double inputRate, double outputRate, int channels, 
                     ConversionQuality quality = ConversionQuality::GOOD);
    
    ~SampleRateAdapter();
    
    /**
     * Process audio samples
     * @param input Input buffer
     * @param inputFrames Number of input frames
     * @param output Output buffer
     * @param outputFrames Available space in output buffer
     * @return Number of output frames produced
     */
    int process(const float* input, int inputFrames, float* output, int outputFrames);
    
    /**
     * Get the ratio between input and output rates
     */
    double getRatio() const;
    
    /**
     * Reset the converter state
     */
    void reset();
    
    /**
     * Check if rates match (no conversion needed)
     */
    bool isPassthrough() const;
    
private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace AES67

#endif // SAMPLE_RATE_ADAPTER_H