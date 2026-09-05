//
// DoPDecoder.h
// AES67 macOS Driver - Build #1
// DSD over PCM (DoP) decoder for DSD64/128/256 support
//

#pragma once

#include "../Shared/Types.h"
#include <cstdint>
#include <vector>

namespace AES67 {

//
// DoP (DSD over PCM) Decoder
//
// Decodes DSD audio data transmitted as PCM containers
// - DSD64: 176.4 kHz PCM container (2.8224 MHz DSD)
// - DSD128: 352.8 kHz PCM container (5.6448 MHz DSD)
// - DSD256: 705.6 kHz PCM container (11.2896 MHz DSD)
//
class DoPDecoder {
public:
    //
    // Check if data is DoP encoded
    // DoP markers: 0x05 or 0xFA in MSB of 24-bit samples
    //
    static bool isDoPStream(const uint8_t* data, size_t sizeBytes);

    //
    // Decode DoP data to native DSD
    //
    // Input: DoP encoded 24-bit PCM samples (big-endian)
    // Output: Native DSD bit stream
    //
    // dopData: DoP encoded data (24-bit samples, big-endian)
    // dopFrames: Number of PCM frames
    // dsdData: Output buffer for DSD data
    //
    static void decode(const uint8_t* dopData, size_t dopFrames, uint8_t* dsdData);

    //
    // Encode DSD to DoP
    //
    // Input: Native DSD bit stream
    // Output: DoP encoded 24-bit PCM samples
    //
    static void encode(const uint8_t* dsdData, size_t dsdFrames, uint8_t* dopData);

    //
    // Get DoP sample rate from DSD rate
    //
    static uint32_t getDoPSampleRate(uint32_t dsdRate);

    //
    // Get DSD rate from DoP sample rate
    //
    static uint32_t getDSDRate(uint32_t dopSampleRate);

    //
    // Validate DoP markers
    //
    static bool validateDoPMarkers(const uint8_t* dopData, size_t dopFrames);

private:
    // DoP markers
    static constexpr uint8_t kDoPMarker1 = 0x05;
    static constexpr uint8_t kDoPMarker2 = 0xFA;

    // Sample rate mappings
    static constexpr uint32_t kDSD64Rate = 2822400;    // 2.8224 MHz
    static constexpr uint32_t kDSD128Rate = 5644800;   // 5.6448 MHz
    static constexpr uint32_t kDSD256Rate = 11289600;  // 11.2896 MHz

    static constexpr uint32_t kDoP64Rate = 176400;     // 176.4 kHz
    static constexpr uint32_t kDoP128Rate = 352800;    // 352.8 kHz
    static constexpr uint32_t kDoP256Rate = 705600;    // 705.6 kHz
};

} // namespace AES67
