//
// RoutingMatrixView.swift
// AES67 Manager
//
// Every NMOS node on the link as one matrix: senders across, receivers
// down, a click on a crosspoint connects or disconnects over IS-05. This
// Mac is one of the nodes. What the local Channel Mapping grid does --
// where each stream lands on this device's channels -- is a different
// thing and stays where it is.
//

import SwiftUI

struct RoutingMatrixView: View {
    @StateObject private var controller = NmosController()
    @Environment(\.dismiss) private var dismiss

    private let cellSize: CGFloat = 28
    private let rowHeaderWidth: CGFloat = 220
    private let columnHeaderHeight: CGFloat = 140

    var body: some View {
        VStack(spacing: 0) {
            header
            Divider()
            if controller.matrix.rows.isEmpty && controller.matrix.columns.isEmpty {
                empty
            } else {
                grid
            }
            Divider()
            footer
        }
        .frame(minWidth: 800, minHeight: 500)
        .onAppear { controller.start() }
        .onDisappear { controller.stop() }
        .alert("Connection refused", isPresented: Binding(
            get: { controller.lastError != nil },
            set: { if !$0 { controller.lastError = nil } })) {
            Button("OK", role: .cancel) {}
        } message: {
            Text(controller.lastError ?? "")
        }
    }

    private var header: some View {
        HStack {
            VStack(alignment: .leading, spacing: 4) {
                Text("Network Routing").font(.title2).fontWeight(.semibold)
                Text("Senders across, receivers down. Click a crosspoint to connect or disconnect over NMOS IS-05.")
                    .font(.caption).foregroundColor(.secondary)
            }
            Spacer()
            Button("Refresh") { controller.refresh() }
            Button("Done") { dismiss() }.keyboardShortcut(.defaultAction)
        }
        .padding()
    }

    private var empty: some View {
        VStack(spacing: 8) {
            Image(systemName: "point.3.connected.trianglepath.dotted").font(.largeTitle).foregroundColor(.secondary)
            Text("No NMOS nodes found yet").font(.headline)
            Text("Nodes announce themselves as _nmos-node._tcp over Bonjour. This Mac appears here once its driver is active.")
                .font(.caption).foregroundColor(.secondary).multilineTextAlignment(.center)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding()
    }

    private var grid: some View {
        ScrollView([.horizontal, .vertical]) {
            VStack(alignment: .leading, spacing: 0) {
                HStack(spacing: 0) {
                    Color.clear.frame(width: rowHeaderWidth, height: columnHeaderHeight)
                    ForEach(controller.matrix.columns) { column in
                        columnHeader(column)
                    }
                }
                ForEach(controller.matrix.rows) { row in
                    HStack(spacing: 0) {
                        rowHeader(row)
                        ForEach(controller.matrix.columns) { column in
                            cell(row: row, column: column)
                        }
                    }
                }
            }
            .padding()
        }
    }

    private func columnHeader(_ column: RoutingMatrix.Column) -> some View {
        VStack(spacing: 2) {
            Spacer()
            Text(column.nodeLabel).font(.caption2).foregroundColor(.secondary).lineLimit(1)
            Text(column.sender.channels.map { "\(column.label) (\($0))" } ?? column.label)
                .font(.caption).lineLimit(1)
            if !column.reachable {
                Text("unreachable").font(.caption2).foregroundColor(.red)
            }
        }
        .fixedSize()
        .rotationEffect(.degrees(-60), anchor: .bottomLeading)
        .frame(width: cellSize, height: columnHeaderHeight, alignment: .bottomLeading)
        .opacity(column.reachable ? 1 : 0.5)
    }

    private func rowHeader(_ row: RoutingMatrix.Row) -> some View {
        HStack(spacing: 6) {
            VStack(alignment: .leading, spacing: 1) {
                Text(row.label).font(.caption).lineLimit(1)
                Text(row.nodeLabel).font(.caption2).foregroundColor(.secondary).lineLimit(1)
            }
            Spacer()
            if !row.reachable {
                Text("unreachable").font(.caption2).foregroundColor(.red)
            } else if !row.writable {
                Text("read-only").font(.caption2).foregroundColor(.secondary)
            }
        }
        .padding(.horizontal, 6)
        .frame(width: rowHeaderWidth, height: cellSize, alignment: .leading)
        .opacity(row.reachable ? 1 : 0.5)
    }

    private func cell(row: RoutingMatrix.Row, column: RoutingMatrix.Column) -> some View {
        let state = controller.matrix.cell(row: row, column: column)
        let pending = controller.inFlight.contains(row.id)
        return Button {
            guard !pending else { return }
            switch state {
            case .off: controller.connect(receiver: row.receiver, to: column.sender)
            case .on: controller.disconnect(receiver: row.receiver)
            case .unavailable: break
            }
        } label: {
            ZStack {
                Rectangle().fill(Color(nsColor: .controlBackgroundColor))
                    .border(Color(nsColor: .separatorColor), width: 0.5)
                if pending {
                    ProgressView().controlSize(.mini)
                } else {
                    switch state {
                    case .on: Circle().fill(Color.accentColor).frame(width: 14, height: 14)
                    case .off: Circle().stroke(Color.secondary, lineWidth: 1).frame(width: 14, height: 14)
                    case .unavailable: Rectangle().fill(Color.gray.opacity(0.15))
                    }
                }
            }
        }
        .buttonStyle(.plain)
        .frame(width: cellSize, height: cellSize)
        .disabled(state == .unavailable || pending)
        .help(state == .on
              ? "Disconnect \(row.label) from \(column.label)"
              : "Connect \(row.label) to \(column.label)")
    }

    private var footer: some View {
        HStack {
            Text("\(controller.nodes.count) node\(controller.nodes.count == 1 ? "" : "s"), "
                 + "\(controller.matrix.columns.count) sender\(controller.matrix.columns.count == 1 ? "" : "s"), "
                 + "\(controller.matrix.rows.count) receiver\(controller.matrix.rows.count == 1 ? "" : "s")")
                .font(.caption).foregroundColor(.secondary)
            Spacer()
            if let error = controller.lastError {
                Text(error).font(.caption).foregroundColor(.red).lineLimit(1)
            }
            if let read = controller.lastRead {
                Text("Read \(read.formatted(date: .omitted, time: .standard))")
                    .font(.caption).foregroundColor(.secondary)
            }
        }
        .padding(8)
    }
}
