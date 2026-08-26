//
// SimpleSDPParse.cpp
// AES67 macOS Driver - Build #4
// Simple example demonstrating SDP parsing
//

#include "Driver/SDPParser.h"
#include <algorithm>
#include <iostream>
#include <iomanip>

using namespace AES67;

void printSeparator() {
    std::cout << std::string(60, '=') << "\n";
}

void printSDPSession(const SDPSession& session) {
    printSeparator();
    std::cout << "Session Name:     " << session.sessionName << "\n";
    if (!session.sessionInfo.empty()) {
        std::cout << "Description:      " << session.sessionInfo << "\n";
    }
    std::cout << "\n";

    std::cout << "Network:\n";
    std::cout << "  Source IP:      " << session.sourceAddress << "\n";
    std::cout << "  Multicast IP:   " << session.connectionAddress << "\n";
    std::cout << "  Port:           " << session.port << "\n";
    std::cout << "\n";

    std::cout << "Audio Format:\n";
    std::cout << "  Encoding:       " << session.encoding << "\n";
    std::cout << "  Sample Rate:    " << session.sampleRate << " Hz\n";
    std::cout << "  Channels:       " << session.numChannels << "\n";
    std::cout << "  Packet Time:    " << (session.ptimeUs / 1000.0) << " ms\n";
    std::cout << "  Samples/Packet: " << session.framecount << "\n";
    std::cout << "\n";

    if (session.ptpDomain >= 0) {
        std::cout << "PTP Synchronization:\n";
        std::cout << "  Domain:         " << session.ptpDomain << "\n";
        if (!session.ptpMasterMAC.empty()) {
            std::cout << "  Master MAC:     " << session.ptpMasterMAC << "\n";
        }
        std::cout << "\n";
    }

    printSeparator();
}

int main(int argc, char* argv[]) {
    std::cout << "\n=== AES67 SDP Parser - Simple Example ===\n\n";

    if (argc < 2) {
        std::cout << "Usage: " << argv[0] << " <sdp_file>\n\n";
        std::cout << "Example SDP files included:\n";
        std::cout << "  - Docs/Examples/riedel_artist_8ch.sdp\n\n";
        return 1;
    }

    std::string filepath = argv[1];
    std::cout << "Parsing: " << filepath << "\n\n";

    // Parse the SDP file
    auto session = SDPParser::parseFile(filepath);

    if (!session) {
        std::cerr << "❌ Failed to parse SDP file\n";
        std::cerr << "Possible reasons:\n";
        std::cerr << "  - File not found\n";
        std::cerr << "  - Invalid SDP format\n";
        std::cerr << "  - Missing required fields\n\n";
        return 1;
    }

    std::cout << "✅ Successfully parsed SDP file\n\n";

    // Print the session details
    printSDPSession(*session);

    // Calculate some useful information
    size_t samplesPerSecond = session->sampleRate * session->numChannels;
    size_t bytesPerSecond;

    if (session->encoding == "L24") {
        bytesPerSecond = samplesPerSecond * 3;  // 24-bit = 3 bytes
    } else if (session->encoding == "L16") {
        bytesPerSecond = samplesPerSecond * 2;  // 16-bit = 2 bytes
    } else {
        bytesPerSecond = 0;
    }

    if (bytesPerSecond > 0) {
        std::cout << "Bandwidth Calculation:\n";
        std::cout << "  Audio Data:     " << std::fixed << std::setprecision(2)
                  << (bytesPerSecond / 1024.0 / 1024.0) << " MB/s\n";

        // Add RTP overhead (12 bytes header + UDP/IP/Ethernet headers ~42 bytes)
        size_t packetsPerSecond = 1000000 / std::max<uint32_t>(session->ptimeUs, 1);
        size_t overheadPerSecond = packetsPerSecond * 54;  // ~54 bytes per packet
        double totalMBps = (bytesPerSecond + overheadPerSecond) / 1024.0 / 1024.0;

        std::cout << "  With Overhead:  " << std::fixed << std::setprecision(2)
                  << totalMBps << " MB/s\n";
        std::cout << "  Packets/sec:    " << packetsPerSecond << "\n";
        std::cout << "\n";
    }

    // Show compatibility
    std::cout << "Compatibility:\n";
    std::cout << "  AES67:          ✓ (by definition)\n";
    std::cout << "  RAVENNA:        ✓ (AES67 subset)\n";

    // Check if it looks like a Dante stream
    if (session->sessionName.find("Dante") != std::string::npos ||
        session->encoding == "L24") {
        std::cout << "  Dante:          ✓ (likely compatible)\n";
    } else {
        std::cout << "  Dante:          ? (may be compatible)\n";
    }

    // Check if it looks like a Riedel stream
    if (session->sessionName.find("Riedel") != std::string::npos ||
        session->sessionName.find("Artist") != std::string::npos ||
        session->sessionInfo.find("Intercom") != std::string::npos) {
        std::cout << "  Riedel Artist:  ✓ (detected)\n";
    }

    std::cout << "\n";

    // Test round-trip generation
    std::cout << "Testing SDP Generation:\n";
    std::string generated = SDPParser::generate(*session);
    std::cout << "  Generated " << generated.length() << " bytes\n";

    // Verify it can be parsed back
    auto reparsed = SDPParser::parseString(generated);
    if (reparsed && reparsed->sessionName == session->sessionName) {
        std::cout << "  ✓ Round-trip verification passed\n";
    } else {
        std::cout << "  ⚠ Round-trip verification failed\n";
    }

    std::cout << "\n";

    return 0;
}
