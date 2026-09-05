//
// DoPDecoder.cpp
// AES67 macOS Driver - Build #6
// DSD over PCM (DoP) decoder for DSD64/128/256 support
//

#include "DoPDecoder.h"
#include <cstring>

namespace AES67 {

bool DoPDecoder::isDoPStream(const uint8_t* data, size_t sizeBytes) {
    // DoP data uses 24-bit samples with markers 0x05 and 0xFA
    // Need at least 2 samples (6 bytes) to detect pattern
    if (!data || sizeBytes < 6) {
        return false;
    }

    // Check for alternating DoP markers in MSB
    // Valid patterns: 0x05, 0xFA, 0x05, 0xFA... or 0xFA, 0x05, 0xFA, 0x05...
    const uint8_t marker1 = data[0]; // MSB of first sample
    const uint8_t marker2 = data[3]; // MSB of second sample (skip 3 bytes)

    return ((marker1 == kDoPMarker1 && marker2 == kDoPMarker2) ||
            (marker1 == kDoPMarker2 && marker2 == kDoPMarker1));
}

void DoPDecoder::decode(const uint8_t* dopData, size_t dopFrames, uint8_t* dsdData) {
    if (!dopData || !dsdData || dopFrames == 0) {
        return;
    }

    // DoP format (24-bit big-endian):
    // [Marker (8 bits)] [DSD byte 1 (8 bits)] [DSD byte 0 (8 bits)]
    //
    // Markers alternate: 0x05, 0xFA, 0x05, 0xFA...
    // Each 24-bit sample contains 16 DSD bits (2 bytes)

    size_t dsdByteIndex = 0;

    for (size_t frame = 0; frame < dopFrames; ++frame) {
        const size_t dopOffset = frame * 3; // 3 bytes per 24-bit sample

        // Extract DSD bytes (ignore marker)
        const uint8_t dsdByte1 = dopData[dopOffset + 1];
        const uint8_t dsdByte0 = dopData[dopOffset + 2];

        // Write DSD bytes to output
        dsdData[dsdByteIndex++] = dsdByte1;
        dsdData[dsdByteIndex++] = dsdByte0;
    }
}

void DoPDecoder::encode(const uint8_t* dsdData, size_t dsdFrames, uint8_t* dopData) {
    if (!dsdData || !dopData || dsdFrames == 0) {
        return;
    }

    // Encode DSD bytes into DoP 24-bit samples
    // Each DoP sample contains: [Marker] [DSD byte 1] [DSD byte 0]

    size_t dsdByteIndex = 0;

    for (size_t frame = 0; frame < dsdFrames; ++frame) {
        const size_t dopOffset = frame * 3;

        // Alternate markers: 0x05, 0xFA, 0x05, 0xFA...
        const uint8_t marker = (frame % 2 == 0) ? kDoPMarker1 : kDoPMarker2;

        // Get DSD bytes
        const uint8_t dsdByte1 = dsdData[dsdByteIndex++];
        const uint8_t dsdByte0 = dsdData[dsdByteIndex++];

        // Build DoP sample (24-bit big-endian)
        dopData[dopOffset + 0] = marker;
        dopData[dopOffset + 1] = dsdByte1;
        dopData[dopOffset + 2] = dsdByte0;
    }
}

uint32_t DoPDecoder::getDoPSampleRate(uint32_t dsdRate) {
    // Map DSD rates to their DoP container rates
    switch (dsdRate) {
        case kDSD64Rate:   // 2.8224 MHz
            return kDoP64Rate;  // 176.4 kHz

        case kDSD128Rate:  // 5.6448 MHz
            return kDoP128Rate; // 352.8 kHz

        case kDSD256Rate:  // 11.2896 MHz
            return kDoP256Rate; // 705.6 kHz

        default:
            return 0; // Unknown rate
    }
}

uint32_t DoPDecoder::getDSDRate(uint32_t dopSampleRate) {
    // Map DoP container rates to DSD rates
    switch (dopSampleRate) {
        case kDoP64Rate:   // 176.4 kHz
            return kDSD64Rate;  // 2.8224 MHz

        case kDoP128Rate:  // 352.8 kHz
            return kDSD128Rate; // 5.6448 MHz

        case kDoP256Rate:  // 705.6 kHz
            return kDSD256Rate; // 11.2896 MHz

        default:
            return 0; // Unknown rate
    }
}

bool DoPDecoder::validateDoPMarkers(const uint8_t* dopData, size_t dopFrames) {
    if (!dopData || dopFrames == 0) {
        return false;
    }

    // Check that markers alternate correctly: 0x05, 0xFA, 0x05, 0xFA...
    for (size_t frame = 0; frame < dopFrames; ++frame) {
        const size_t dopOffset = frame * 3;
        const uint8_t marker = dopData[dopOffset];

        // Expected marker alternates
        const uint8_t expectedMarker = (frame % 2 == 0) ? kDoPMarker1 : kDoPMarker2;

        if (marker != expectedMarker) {
            return false; // Invalid marker sequence
        }
    }

    return true;
}

} // namespace AES67
