//
// DeviceListView.swift
// AES67 Controller
//
// The device list: what is on the network, what each one offers, and which
// clock it follows.
//
// It is the first screen of every routing application there is, and the clock
// column is not decoration: a device that is not locked to the same
// grandmaster as the rest will pass audio and drift, which looks like a
// routing fault and is not one.
//

import SwiftUI

struct DeviceListView: View {
    @ObservedObject var nmos: NmosController

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            header
            Divider()
            if nmos.nodes.isEmpty {
                notice("No NMOS nodes found yet. Devices are discovered over mDNS as _nmos-node._tcp; a plant with a registry on another subnet needs one to be reachable from here.")
            } else {
                Table(nmos.nodes) {
                    TableColumn("Device") { node in
                        VStack(alignment: .leading, spacing: 2) {
                            Text(node.label)
                            Text("\(node.host):\(node.port)")
                                .font(.caption)
                                .foregroundColor(.secondary)
                        }
                    }
                    TableColumn("Senders") { node in Text("\(node.senders.count)") }
                    TableColumn("Receivers") { node in Text("\(node.receivers.count)") }
                    TableColumn("Clock") { node in
                        Text(node.clockSummary)
                            .foregroundColor(node.clocks.first?.locked == false ? .orange : .primary)
                    }
                    TableColumn("Routing") { node in
                        // What this device will actually let a controller do.
                        // IS-05 is stream routing; IS-08 is the channel grid,
                        // and most gear has the first and not the second.
                        Text(capabilities(of: node))
                            .font(.caption)
                            .foregroundColor(.secondary)
                    }
                    TableColumn("State") { node in
                        Text(node.reachable ? "Reachable" : "Unreachable")
                            .foregroundColor(node.reachable ? .primary : .red)
                    }
                }
            }
        }
    }

    private func capabilities(of node: NmosNode) -> String {
        var parts: [String] = []
        if node.connectionRoot != nil { parts.append("IS-05") }
        if node.channelMappingRoot != nil { parts.append("IS-08") }
        return parts.isEmpty ? "read-only" : parts.joined(separator: " + ")
    }

    private var header: some View {
        HStack {
            VStack(alignment: .leading, spacing: 2) {
                Text("Devices")
                    .font(.title3)
                    .fontWeight(.semibold)
                Text(nmos.lastRead.map { "Last read \($0.formatted(date: .omitted, time: .standard))" }
                     ?? "Reading…")
                    .font(.caption)
                    .foregroundColor(.secondary)
            }
            Spacer()
            Button("Refresh") { nmos.refresh() }
        }
        .padding()
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
