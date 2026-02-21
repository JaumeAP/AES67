//
// StreamInfo.swift
// AES67 Manager - Build #15
// Data models for streams, mappings, and audio levels
//

import Foundation

struct StreamInfo: Identifiable, Hashable {
    let id: UUID
    var name: String
    var description: String?

    // Network
    var multicastIP: String
    var port: UInt16
    var sourceIP: String?
    var ttl: UInt8 = 32

    // Audio format
    var encoding: String  // "L16" or "L24"
    var sampleRate: UInt32
    var numChannels: UInt16
    var payloadType: UInt8 = 97

    // PTP
    var ptpDomain: Int = 0

    // Status
    var isActive: Bool = true
    var isConnected: Bool = false
    var startTime: Date?

    // Mapping
    var mapping: ChannelMappingInfo?

    // Statistics
    var statistics: StreamStatistics?

    func hash(into hasher: inout Hasher) {
        hasher.combine(id)
    }

    static func == (lhs: StreamInfo, rhs: StreamInfo) -> Bool {
        lhs.id == rhs.id
    }
}

struct ChannelMappingInfo: Codable {
    var streamID: UUID
    var streamName: String
    var streamChannelCount: UInt16
    var streamChannelOffset: UInt16 = 0
    var deviceChannelStart: UInt16
    var deviceChannelCount: UInt16
}

struct StreamStatistics {
    var packetsReceived: UInt64 = 0
    var packetsLost: UInt64 = 0
    var lossPercentage: Double = 0.0
    var bytesReceived: UInt64 = 0
    var bytesSent: UInt64 = 0
    var underruns: UInt64 = 0
    var overruns: UInt64 = 0
    var malformedPackets: UInt64 = 0
}

// MARK: - Audio Level Data

/// Represents audio level information for a single channel
struct ChannelLevelInfo: Identifiable {
    let id: Int  // Channel index (0-based)
    var level: Float  // Current level (0.0 to 1.0, linear scale)
    var peak: Float   // Peak hold level (0.0 to 1.0)
    var levelDB: Float  // Level in dB (typically -60 to 0)

    /// Threshold for "signal present" detection (-60dB)
    static let signalPresentThresholdDB: Float = -60.0

    /// Returns true if signal is present (above -60dB)
    var hasSignal: Bool {
        return levelDB > Self.signalPresentThresholdDB
    }

    /// Converts dB value to linear (0.0 to 1.0) for display
    static func dbToLinear(_ db: Float) -> Float {
        // Map -60dB to 0dB onto 0.0 to 1.0
        let clampedDB = max(-60.0, min(0.0, db))
        return (clampedDB + 60.0) / 60.0
    }

    /// Converts linear (0.0 to 1.0) to dB
    static func linearToDB(_ linear: Float) -> Float {
        guard linear > 0 else { return -60.0 }
        return max(-60.0, 20.0 * log10(linear))
    }
}

/// Audio level data for a stream
struct StreamAudioLevels {
    var channelLevels: [ChannelLevelInfo]
    var lastUpdate: Date

    /// Returns true if any channel has signal above -60dB
    var hasAnySignal: Bool {
        return channelLevels.contains { $0.hasSignal }
    }

    /// Returns the number of channels with active signal
    var activeChannelCount: Int {
        return channelLevels.filter { $0.hasSignal }.count
    }

    /// Creates silent placeholder data for when no real driver data is available
    static func silentData(channelCount: Int, isConnected: Bool) -> StreamAudioLevels {
        var levels: [ChannelLevelInfo] = []
        for i in 0..<channelCount {
            levels.append(ChannelLevelInfo(
                id: i,
                level: 0,
                peak: 0,
                levelDB: -60.0
            ))
        }
        return StreamAudioLevels(channelLevels: levels, lastUpdate: Date())
    }

    /// Creates mock data for UI testing
    static func mockData(channelCount: Int) -> StreamAudioLevels {
        var levels: [ChannelLevelInfo] = []
        for i in 0..<channelCount {
            // Generate varying mock levels
            let baseLevel = Float.random(in: 0.1...0.8)
            let variation = Float.random(in: -0.1...0.1)
            let level = max(0, min(1, baseLevel + variation))
            let peakLevel = min(1.0, level + Float.random(in: 0.05...0.15))
            let levelDB = ChannelLevelInfo.linearToDB(level)

            levels.append(ChannelLevelInfo(
                id: i,
                level: level,
                peak: peakLevel,
                levelDB: levelDB
            ))
        }
        return StreamAudioLevels(channelLevels: levels, lastUpdate: Date())
    }
}

// MARK: - Example Data for Previews

extension StreamInfo {
    static var example: StreamInfo {
        StreamInfo(
            id: UUID(),
            name: "Riedel Artist Panel 1",
            description: "Intercom system - 8 channels",
            multicastIP: "239.0.0.1",
            port: 5004,
            sourceIP: "192.168.1.100",
            encoding: "L24",
            sampleRate: 48000,
            numChannels: 8,
            isConnected: true,
            startTime: Date(),
            mapping: ChannelMappingInfo(
                streamID: UUID(),
                streamName: "Riedel Artist Panel 1",
                streamChannelCount: 8,
                deviceChannelStart: 0,
                deviceChannelCount: 8
            ),
            statistics: StreamStatistics(
                packetsReceived: 150000,
                packetsLost: 12,
                lossPercentage: 0.008,
                bytesReceived: 36_000_000,
                underruns: 0,
                overruns: 0
            )
        )
    }
}

// MARK: - Codable Configuration

struct DriverConfiguration: Codable {
    var version: String = "1.0.7"
    var streams: [StreamConfig] = []
    var mappings: [ChannelMappingInfo] = []
}

struct StreamConfig: Codable {
    var id: String
    var name: String
    var description: String?
    var multicastIP: String
    var port: UInt16
    var encoding: String
    var sampleRate: UInt32
    var numChannels: UInt16
    var payloadType: UInt8
    var ptpDomain: Int
}
