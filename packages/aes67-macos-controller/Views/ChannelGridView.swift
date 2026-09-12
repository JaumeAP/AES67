//
// ChannelGridView.swift
// AES67 Controller
//
// The grid, channel by channel: IS-08.
//
// The crosspoint matrix routes whole streams -- a sender onto a receiver,
// which is IS-05 and what most gear supports. This is the finer one a routing
// application is really asked for: which INPUT CHANNEL feeds which OUTPUT
// CHANNEL inside one device. Inputs across, outputs down, a click on a
// crosspoint sets it and a click on the one already set mutes it.
//
// Only devices that declare an IS-08 control appear here. That is not most of
// them, and the picker says so rather than showing an empty grid.
//

import SwiftUI

struct ChannelGridView: View {
    @ObservedObject var nmos: NmosController

    @State private var selectedNodeId: String = ""
    @State private var map: NmosChannelMap = .empty
    @State private var loading = false

    private var mappableNodes: [NmosNode] {
        nmos.nodes.filter { $0.channelMappingRoot != nil }
    }

    private var selectedNode: NmosNode? {
        mappableNodes.first { $0.id == selectedNodeId } ?? mappableNodes.first
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            header
            Divider()
            if mappableNodes.isEmpty {
                notice("No device here maps channels. IS-08 is optional, and gear that routes "
                     + "whole streams and nothing finer declares no channel mapping control — "
                     + "use the Routing tab for those.")
            } else if map.outputs.isEmpty {
                notice(loading ? "Reading the map…" : "This device declares a channel map with no outputs.")
            } else {
                grid
            }
        }
        .onAppear { load() }
        .onChange(of: selectedNodeId) { _ in load() }
    }

    private var header: some View {
        HStack {
            VStack(alignment: .leading, spacing: 2) {
                Text("Channels")
                    .font(.title3)
                    .fontWeight(.semibold)
                Text("Inputs across, outputs down. A click sets a crosspoint; clicking the one "
                   + "already set mutes that output channel.")
                    .font(.caption)
                    .foregroundColor(.secondary)
            }
            Spacer()
            if !mappableNodes.isEmpty {
                Picker("Device", selection: Binding(get: { selectedNode?.id ?? "" },
                                                    set: { selectedNodeId = $0 })) {
                    ForEach(mappableNodes) { node in
                        Text(node.label).tag(node.id)
                    }
                }
                .pickerStyle(.menu)
                .frame(maxWidth: 240)
            }
            Button("Refresh") { load() }
        }
        .padding()
    }

    private var grid: some View {
        ScrollView([.horizontal, .vertical]) {
            VStack(alignment: .leading, spacing: 0) {
                HStack(spacing: 0) {
                    Text("")
                        .frame(width: 220, alignment: .leading)
                    ForEach(inputChannels, id: \.key) { entry in
                        Text(entry.label)
                            .font(.caption)
                            .rotationEffect(.degrees(-60))
                            .frame(width: 34, height: 110, alignment: .bottom)
                    }
                }
                ForEach(outputChannels, id: \.key) { output in
                    HStack(spacing: 0) {
                        Text(output.label)
                            .font(.caption)
                            .frame(width: 220, alignment: .leading)
                        ForEach(inputChannels, id: \.key) { input in
                            crosspoint(output: output, input: input)
                        }
                    }
                }
            }
            .padding()
        }
    }

    /// One flattened axis entry: the port it belongs to and the channel in it.
    private struct Axis {
        let key: String
        let label: String
        let portId: String
        let channel: Int
    }

    private var inputChannels: [Axis] {
        map.inputs.flatMap { port in
            port.channels.map { channel in
                Axis(key: "\(port.id)/\(channel.index)",
                     label: "\(port.label) · \(channel.label)",
                     portId: port.id, channel: channel.index)
            }
        }
    }

    private var outputChannels: [Axis] {
        map.outputs.flatMap { port in
            port.channels.map { channel in
                Axis(key: "\(port.id)/\(channel.index)",
                     label: "\(port.label) · \(channel.label)",
                     portId: port.id, channel: channel.index)
            }
        }
    }

    private func crosspoint(output: Axis, input: Axis) -> some View {
        let current = map.source(ofOutput: output.portId, channel: output.channel)
        let connected = current?.input == input.portId && current?.channel == input.channel
        return Button {
            set(output: output, input: input, clearing: connected)
        } label: {
            ZStack {
                Rectangle()
                    .strokeBorder(Color.secondary.opacity(0.3), lineWidth: 0.5)
                    .background(Rectangle().fill(connected ? Color.accentColor : Color.clear))
                if connected {
                    Image(systemName: "checkmark")
                        .font(.system(size: 9, weight: .bold))
                        .foregroundColor(.white)
                }
            }
            .frame(width: 34, height: 26)
        }
        .buttonStyle(.plain)
        .help(connected ? "Mute \(output.label)" : "Feed \(output.label) from \(input.label)")
    }

    private func set(output: Axis, input: Axis, clearing: Bool) {
        guard let node = selectedNode else { return }
        Task {
            await nmos.setCrosspoint(on: node,
                                     output: output.portId, outputChannel: output.channel,
                                     input: clearing ? nil : input.portId,
                                     inputChannel: clearing ? nil : input.channel)
            load()
        }
    }

    private func load() {
        guard let node = selectedNode else {
            map = .empty
            return
        }
        loading = true
        Task {
            let read = await nmos.channelMap(of: node)
            await MainActor.run {
                map = read
                loading = false
            }
        }
    }

    private func notice(_ text: String) -> some View {
        Text(text)
            .font(.callout)
            .foregroundColor(.secondary)
            .multilineTextAlignment(.center)
            .fixedSize(horizontal: false, vertical: true)
            .padding(40)
            .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}
