//
// ChannelMapDiagnosticView.swift
// AES67 Manager - Build #18
// Channel Mapping Diagnostic and Visualization Tool
//

import SwiftUI
import AppKit
import UniformTypeIdentifiers

struct ChannelMapDiagnosticView: View {
    @EnvironmentObject var driverManager: DriverManager
    @State private var selectedChannel: Int? = nil

    // Visual grid configuration
    private let columns = 16  // 16 columns × 8 rows = 128 channels
    private let channelSize: CGFloat = 40

    var body: some View {
        VStack(alignment: .leading, spacing: 20) {
            // Header
            HStack {
                VStack(alignment: .leading) {
                    Text("Channel Mapping Diagnostic")
                        .font(.title)
                        .fontWeight(.semibold)

                    Text("128-channel device visualization")
                        .font(.caption)
                        .foregroundColor(.secondary)
                }

                Spacer()

                // Statistics
                VStack(alignment: .trailing, spacing: 4) {
                    Text("Used: \(usedChannelCount)/128")
                        .font(.callout)
                    Text("Free: \(128 - usedChannelCount)")
                        .font(.callout)
                        .foregroundColor(.green)
                }
                .padding(.horizontal)

                Button("Export Map") {
                    exportChannelMap()
                }
                .buttonStyle(.bordered)
            }
            .padding()

            Divider()

            ScrollView {
                VStack(alignment: .leading, spacing: 20) {
                    // Channel grid
                    channelGridView

                    Divider()

                    // Stream assignments
                    streamAssignmentsView

                    // Selected channel info
                    if let channel = selectedChannel {
                        Divider()
                        selectedChannelInfoView(channel: channel)
                    }
                }
                .padding()
            }
        }
        .frame(minWidth: 900, minHeight: 700)
    }

    // MARK: - Channel Grid View

    private var channelGridView: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Device Channel Map (0-127)")
                .font(.headline)

            Text("Click a channel to see details")
                .font(.caption)
                .foregroundColor(.secondary)

            LazyVGrid(columns: Array(repeating: GridItem(.fixed(channelSize), spacing: 2), count: columns), spacing: 2) {
                ForEach(0..<128, id: \.self) { channel in
                    channelBox(for: channel)
                        .onTapGesture {
                            selectedChannel = channel
                        }
                }
            }

            // Legend
            HStack(spacing: 20) {
                legendItem(color: .gray, label: "Unassigned")
                legendItem(color: .blue, label: "Input Stream")
                legendItem(color: .green, label: "Output Stream")
                legendItem(color: .yellow, label: "Selected")
            }
            .padding(.top, 10)
        }
    }

    private func channelBox(for channel: Int) -> some View {
        let streamInfo = getStreamForChannel(channel)
        let isSelected = selectedChannel == channel
        let color: Color = {
            if isSelected {
                return .yellow
            } else if let info = streamInfo {
                return info.isInput ? .blue : .green
            } else {
                return .gray
            }
        }()

        return ZStack {
            RoundedRectangle(cornerRadius: 4)
                .fill(color.opacity(0.3))
                .overlay(
                    RoundedRectangle(cornerRadius: 4)
                        .stroke(color, lineWidth: isSelected ? 2 : 1)
                )

            Text("\(channel)")
                .font(.system(size: 10, weight: isSelected ? .bold : .regular))
                .foregroundColor(.primary)
        }
        .frame(width: channelSize, height: channelSize)
        .help(streamInfo?.name ?? "Unassigned")
    }

    private func legendItem(color: Color, label: String) -> some View {
        HStack(spacing: 4) {
            RoundedRectangle(cornerRadius: 2)
                .fill(color.opacity(0.3))
                .frame(width: 16, height: 16)
                .overlay(
                    RoundedRectangle(cornerRadius: 2)
                        .stroke(color, lineWidth: 1)
                )
            Text(label)
                .font(.caption)
        }
    }

    // MARK: - Stream Assignments View

    private var streamAssignmentsView: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Stream Channel Assignments")
                .font(.headline)

            if driverManager.streams.isEmpty {
                Text("No streams configured")
                    .foregroundColor(.secondary)
                    .italic()
            } else {
                ForEach(driverManager.streams) { stream in
                    streamAssignmentRow(for: stream)
                }
            }
        }
    }

    private func streamAssignmentRow(for stream: StreamInfo) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(stream.name)
                    .font(.subheadline)
                    .fontWeight(.medium)

                Spacer()

                Text("\(stream.numChannels) channels")
                    .font(.caption)
                    .foregroundColor(.secondary)
            }

            if let mapping = stream.mapping {
                HStack {
                    Text("Device Channels:")
                        .font(.caption)
                        .foregroundColor(.secondary)

                    Text("\(mapping.deviceChannelStart) - \(mapping.deviceChannelStart + mapping.deviceChannelCount - 1)")
                        .font(.caption)
                        .fontWeight(.medium)

                    Spacer()

                    Text("Stream Channels:")
                        .font(.caption)
                        .foregroundColor(.secondary)

                    Text("\(mapping.streamChannelOffset) - \(mapping.streamChannelOffset + mapping.streamChannelCount - 1)")
                        .font(.caption)
                        .fontWeight(.medium)
                }
            } else {
                Text("No mapping configured")
                    .font(.caption)
                    .foregroundColor(.orange)
            }
        }
        .padding()
        .background(Color.secondary.opacity(0.1))
        .cornerRadius(8)
    }

    // MARK: - Selected Channel Info

    private func selectedChannelInfoView(channel: Int) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Channel \(channel) Details")
                .font(.headline)

            if let streamInfo = getStreamForChannel(channel) {
                VStack(alignment: .leading, spacing: 6) {
                    infoRow(label: "Stream", value: streamInfo.name)
                    infoRow(label: "Direction", value: streamInfo.isInput ? "Input (Receive)" : "Output (Transmit)")
                    infoRow(label: "Stream Address", value: "\(streamInfo.multicastIP):\(streamInfo.port)")

                    if let mapping = streamInfo.mapping {
                        let streamCh = channel - Int(mapping.deviceChannelStart) + Int(mapping.streamChannelOffset)
                        infoRow(label: "Stream Channel", value: "\(streamCh)")
                        infoRow(label: "Total Channels", value: "\(mapping.streamChannelCount)")
                    }
                }
                .padding()
                .background(Color.blue.opacity(0.1))
                .cornerRadius(8)
            } else {
                Text("This channel is unassigned")
                    .foregroundColor(.secondary)
                    .italic()
                    .padding()
            }
        }
    }

    private func infoRow(label: String, value: String) -> some View {
        HStack {
            Text("\(label):")
                .font(.caption)
                .foregroundColor(.secondary)
                .frame(width: 120, alignment: .leading)

            Text(value)
                .font(.caption)
                .fontWeight(.medium)
        }
    }

    // MARK: - Helper Functions

    private struct ChannelStreamInfo {
        let name: String
        let isInput: Bool
        let multicastIP: String
        let port: UInt16
        let mapping: ChannelMappingInfo?
    }

    private func getStreamForChannel(_ channel: Int) -> ChannelStreamInfo? {
        for stream in driverManager.streams {
            if let mapping = stream.mapping {
                let start = Int(mapping.deviceChannelStart)
                let count = Int(mapping.deviceChannelCount)
                if channel >= start && channel < start + count {
                    return ChannelStreamInfo(
                        name: stream.name,
                        isInput: true,  // TODO: Determine from stream type
                        multicastIP: stream.multicastIP,
                        port: stream.port,
                        mapping: mapping
                    )
                }
            }
        }
        return nil
    }

    private var usedChannelCount: Int {
        var used = Set<Int>()
        for stream in driverManager.streams {
            if let mapping = stream.mapping {
                let start = Int(mapping.deviceChannelStart)
                let count = Int(mapping.deviceChannelCount)
                for ch in start..<(start + count) {
                    used.insert(ch)
                }
            }
        }
        return used.count
    }

    private func exportChannelMap() {
        var mappingData: [String: Any] = [:]
        mappingData["totalChannels"] = 128
        mappingData["usedChannels"] = usedChannelCount
        mappingData["freeChannels"] = 128 - usedChannelCount

        var streams: [[String: Any]] = []
        for stream in driverManager.streams {
            if let mapping = stream.mapping {
                streams.append([
                    "name": stream.name,
                    "deviceChannelStart": mapping.deviceChannelStart,
                    "deviceChannelCount": mapping.deviceChannelCount,
                    "streamChannelOffset": mapping.streamChannelOffset,
                    "streamChannelCount": mapping.streamChannelCount
                ])
            }
        }
        mappingData["streams"] = streams

        guard let data = try? JSONSerialization.data(withJSONObject: mappingData, options: .prettyPrinted) else {
            return
        }

        let panel = NSSavePanel()
        panel.allowedContentTypes = [UTType.json]
        panel.nameFieldStringValue = "AES67_ChannelMap.json"
        panel.message = "Export channel mapping"

        panel.begin { response in
            if response == .OK, let url = panel.url {
                try? data.write(to: url, options: .atomic)
            }
        }
    }
}

// MARK: - Preview

struct ChannelMapDiagnosticView_Previews: PreviewProvider {
    static var previews: some View {
        ChannelMapDiagnosticView()
            .environmentObject(DriverManager())
    }
}
