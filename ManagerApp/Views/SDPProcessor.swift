import Foundation
import SwiftUI
import UniformTypeIdentifiers

/// Handles parsing and processing of SDP files for AES67 stream configuration
class SDPProcessor {

    /// Represents an AES67 stream configuration parsed from SDP
    struct AES67StreamConfig {
        let connectionAddress: String
        let port: Int
        let ptpDomain: Int
        let sampleRate: Int
        let channels: Int
        let streamName: String
        let payloadType: Int

        init(connectionAddress: String, port: Int, ptpDomain: Int,
             sampleRate: Int, channels: Int, streamName: String, payloadType: Int) {
            self.connectionAddress = connectionAddress
            self.port = port
            self.ptpDomain = ptpDomain
            self.sampleRate = sampleRate
            self.channels = channels
            self.streamName = streamName
            self.payloadType = payloadType
        }
    }

    /// Parses an SDP string and extracts AES67 stream configuration
    /// - Parameter sdpContent: The SDP content as a string
    /// - Returns: An AES67StreamConfig object or nil if parsing fails
    static func parseSDP(_ sdpContent: String) -> AES67StreamConfig? {
        let lines = sdpContent.components(separatedBy: .newlines)

        var connectionAddress: String?
        var port: Int?
        var ptpDomain: Int = 0  // Default domain
        var sampleRate: Int = 48000  // Default sample rate
        let channels: Int = 2  // Default channels
        var streamName: String = "Unknown Stream"
        var payloadType: Int = 96  // Default dynamic payload type

        for line in lines {
            let trimmedLine = line.trimmingCharacters(in: .whitespacesAndNewlines)

            if trimmedLine.hasPrefix("c=") {  // Connection information
                // Expected format: c=IN IP4 239.69.0.1
                let parts = trimmedLine.components(separatedBy: " ")
                if parts.count >= 3 {
                    connectionAddress = parts[2]
                }
            } else if trimmedLine.hasPrefix("m=") {  // Media information
                // Expected format: m=audio 5004 RTP/AVP 96
                let parts = trimmedLine.components(separatedBy: " ")
                if parts.count >= 4 {
                    if let parsedPort = Int(parts[1]) {
                        port = parsedPort
                    }
                    if let parsedPayloadType = Int(parts[3]) {
                        payloadType = parsedPayloadType
                    }
                }
            } else if trimmedLine.hasPrefix("a=ts-refclk:") {  // PTP clock reference
                // Expected format: a=ts-refclk:ptp=IEEE1588-2008:00-00-00-00-00-00-00-00:0
                if trimmedLine.contains("ptp=IEEE1588-2008") {
                    let parts = trimmedLine.components(separatedBy: ":")
                    if parts.count >= 4 {
                        // The domain is typically the last part
                        if let domain = Int(parts[parts.count - 1]) {
                            ptpDomain = domain
                        }
                    }
                }
            } else if trimmedLine.hasPrefix("a=mediaclk:") {  // Media clock
                // Expected format: a=mediaclk:direct=0
                if trimmedLine.contains("rate=") {
                    let rateParts = trimmedLine.components(separatedBy: "rate=")
                    if rateParts.count > 1 {
                        let rateStr = rateParts[1].components(separatedBy: CharacterSet.whitespacesAndNewlines)[0]
                        if let parsedRate = Int(rateStr) {
                            sampleRate = parsedRate
                        }
                    }
                }
            } else if trimmedLine.hasPrefix("s=") {  // Session name
                let namePart = trimmedLine.components(separatedBy: "=")
                if namePart.count > 1 {
                    streamName = namePart[1]
                }
            }
        }

        // Check if we have the required information
        guard let validConnectionAddress = connectionAddress, let validPort = port else {
            return nil
        }

        return AES67StreamConfig(
            connectionAddress: validConnectionAddress,
            port: validPort,
            ptpDomain: ptpDomain,
            sampleRate: sampleRate,
            channels: channels,
            streamName: streamName,
            payloadType: payloadType
        )
    }
}

/// View modifier to handle SDP file drops
struct SDPDropDelegate: DropDelegate {
    let onDrop: (URL) -> Void

    func performDrop(info: DropInfo) -> Bool {
        let providers = info.itemProviders(for: [.fileURL, .plainText])
        guard !providers.isEmpty else { return false }

        for item in providers {
            if item.hasItemConformingToTypeIdentifier(UTType.fileURL.identifier) {
                _ = item.loadObject(ofClass: URL.self) { url, _ in
                    guard let fileURL = url else { return }

                    // Verify it's an SDP file
                    if fileURL.pathExtension.lowercased() == "sdp" {
                        DispatchQueue.main.async {
                            self.onDrop(fileURL)
                        }
                    }
                }
            } else if item.hasItemConformingToTypeIdentifier(UTType.plainText.identifier) {
                _ = item.loadObject(ofClass: NSString.self) { string, _ in
                    guard let sdpContent = string as? String else { return }

                    // Create a temporary file with the SDP content
                    let tempDir = FileManager.default.temporaryDirectory
                    let tempFileURL = tempDir.appendingPathComponent("temp.sdp")

                    do {
                        try sdpContent.write(to: tempFileURL, atomically: true, encoding: .utf8)
                        DispatchQueue.main.async {
                            self.onDrop(tempFileURL)
                        }
                    } catch {
                        print("Failed to create temporary SDP file: \(error)")
                    }
                }
            }
        }

        return true
    }
}

/// Extension to add SDP parsing capability to the StreamListView
extension StreamListView {

    /// Process an SDP file and add as a stream via DriverManager
    func processSDPFile(_ fileURL: URL) {
        do {
            let sdpContent = try String(contentsOf: fileURL, encoding: .utf8)

            if let streamConfig = SDPProcessor.parseSDP(sdpContent) {
                driverManager.addStream(
                    name: streamConfig.streamName,
                    multicastIP: streamConfig.connectionAddress,
                    port: UInt16(streamConfig.port),
                    numChannels: UInt16(streamConfig.channels),
                    sampleRate: UInt32(streamConfig.sampleRate),
                    encoding: "L24",
                    ptpDomain: streamConfig.ptpDomain
                )
            } else {
                print("Failed to parse SDP file: \(fileURL)")
            }
        } catch {
            print("Error reading SDP file: \(error)")
        }
    }
}
