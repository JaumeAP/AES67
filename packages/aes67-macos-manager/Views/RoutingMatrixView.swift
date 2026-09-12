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
    /// Destinations that answer to nobody: gear with no control protocol,
    /// which a crosspoint reaches by re-addressing the SOURCE. Empty in the
    /// Manager, where the only thing being routed is this machine; the
    /// Controller passes what its PTP observer found.
    var fixedSinks: [FixedSink] = []

    /// Sources that answer to nobody either: a Dolby processor is a PTP
    /// master and what it sends is set on the unit. A crosspoint between one
    /// of these and a fixed sink has no end that can be told anything, and
    /// the matrix says so instead of offering a button.
    var fixedSources: [FixedSource] = []

    @StateObject private var controller = NmosController()
    @Environment(\.dismiss) private var dismiss

    private let cellSize: CGFloat = 32
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
                    ForEach(fixedSources) { source in
                        fixedSourceHeader(source)
                    }
                }
                ForEach(controller.matrix.rows) { row in
                    HStack(spacing: 0) {
                        rowHeader(row)
                        ForEach(controller.matrix.columns) { column in
                            cell(row: row, column: column)
                        }
                        ForEach(fixedSources) { source in
                            fixedSourceCell(row: row, source: source)
                        }
                    }
                }
                ForEach(fixedSinks) { sink in
                    HStack(spacing: 0) {
                        fixedSinkHeader(sink)
                        ForEach(controller.matrix.columns) { column in
                            fixedSinkCell(sink: sink, column: column)
                        }
                        ForEach(fixedSources) { source in
                            fixedPairCell(sink: sink, source: source)
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
                .font(.caption)
                .foregroundColor(column.reachable ? .primary : .red)
                .lineLimit(1)
        }
        .fixedSize()
        .rotationEffect(.degrees(-90), anchor: .bottomLeading)
        .frame(width: cellSize, height: columnHeaderHeight, alignment: .bottomLeading)
        .opacity(column.reachable ? 1 : 0.5)
        .help(column.reachable ? "" : "unreachable")
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

    private func fixedSourceHeader(_ source: FixedSource) -> some View {
        VStack(spacing: 2) {
            Spacer()
            Text("fixed source").font(.caption2).foregroundColor(.secondary).lineLimit(1)
            Text(source.label).font(.caption).lineLimit(1)
        }
        .fixedSize()
        .rotationEffect(.degrees(-90), anchor: .bottomLeading)
        .frame(width: cellSize, height: columnHeaderHeight, alignment: .bottomLeading)
        .help(source.note)
    }

    /// One of our receivers against a source that answers nothing: the
    /// receiver is the end that can be told where to listen, so this is
    /// programmable -- and stays so.
    private func fixedSourceCell(row: RoutingMatrix.Row, source: FixedSource) -> some View {
        let pending = controller.inFlight.contains(row.id)
        return Button {
            guard !pending, row.writable, row.reachable else { return }
            controller.listen(receiver: row.receiver,
                              at: source.multicastAddress, port: source.port,
                              label: source.label)
        } label: {
            ZStack {
                Rectangle()
                    .strokeBorder(Color.secondary.opacity(0.3), lineWidth: 0.5)
                    .background(Rectangle().fill(Color.clear))
                if pending {
                    ProgressView().controlSize(.mini)
                } else {
                    Image(systemName: "arrow.left")
                        .font(.system(size: 9))
                        .foregroundColor(.secondary)
                }
            }
            .frame(width: cellSize, height: cellSize)
        }
        .buttonStyle(.plain)
        .disabled(!row.writable || !row.reachable)
        .help(row.writable
              ? "Point \(row.label) at \(source.label): \(source.multicastAddress):\(source.port). That device answers nothing, so this end holds the connection — and can be pointed elsewhere later."
              : "\(row.label) is read-only: its node serves no connection API.")
    }

    private func fixedSinkHeader(_ sink: FixedSink) -> some View {
        HStack(spacing: 6) {
            VStack(alignment: .leading, spacing: 1) {
                Text(sink.label).font(.caption).lineLimit(1)
                Text("\(sink.multicastAddress):\(sink.port)")
                    .font(.caption2).foregroundColor(.secondary).lineLimit(1)
            }
            Spacer()
            if let feeding = FixedSinkRouting.senderFeeding(sink, among: allSenders) {
                Text("fed by \(feeding.label)").font(.caption2).foregroundColor(.secondary)
                    .lineLimit(1)
            } else {
                Text("free").font(.caption2).foregroundColor(.secondary)
            }
        }
        .padding(.horizontal, 6)
        .frame(width: rowHeaderWidth, height: cellSize, alignment: .leading)
        .help(sink.note)
    }

    /// Every sender on every node, which is what a fixed sink's row is read
    /// against: the device cannot say what it is receiving, so the evidence
    /// is which sender is addressed at it.
    private var allSenders: [NmosSender] {
        controller.nodes.flatMap { $0.senders }
    }

    /// A crosspoint onto a fixed sink patches the SENDER: the destination
    /// cannot be told anything, so what changes is where the source
    /// transmits.
    ///
    /// And once it is set, the row is settled. Unpicking it would mean
    /// telling that device something, and there is nothing there to tell --
    /// its address and ports are on a sticker, not in an API. So a taken row
    /// shows who feeds it and takes no more clicks; a free row takes one, to
    /// make the connection that will then be fixed.
    /// A crosspoint between one of our senders and a fixed sink. The sink
    /// cannot be told anything, so the sender is the end that holds the
    /// connection -- and holding it is not freezing it: clicking the one that
    /// feeds stops it, clicking another moves the feed there.
    private func fixedSinkCell(sink: FixedSink, column: RoutingMatrix.Column) -> some View {
        let feeding = FixedSinkRouting.senderFeeding(sink, among: allSenders)
        let isThisSender = feeding?.id == column.sender.id
        let pending = controller.inFlight.contains(column.sender.id)
        return Button {
            guard !pending, column.reachable else { return }
            if isThisSender {
                controller.stop(sender: column.sender)
            } else {
                // Moving the feed: the sender that had it is stopped first and
                // the move waits for that, because two senders on one
                // destination is a collision and not a route.
                controller.move(from: feeding, to: column.sender,
                                destination: sink.multicastAddress, port: sink.port)
            }
        } label: {
            ZStack {
                Rectangle()
                    .strokeBorder(Color.secondary.opacity(0.3), lineWidth: 0.5)
                    .background(Rectangle().fill(isThisSender ? Color.accentColor : Color.clear))
                if pending {
                    ProgressView().controlSize(.mini)
                } else if isThisSender {
                    Image(systemName: "checkmark")
                        .font(.system(size: 9, weight: .bold))
                        .foregroundColor(.white)
                } else {
                    Image(systemName: "arrow.right")
                        .font(.system(size: 9))
                        .foregroundColor(.secondary)
                }
            }
            .frame(width: cellSize, height: cellSize)
        }
        .buttonStyle(.plain)
        .disabled(!column.reachable)
        .help(helpText(sink: sink, column: column, feeding: feeding, isThisSender: isThisSender))
    }

    /// A crosspoint between two devices that answer to nobody. Nothing here
    /// can be configured from anywhere: both ends are set on their own units.
    private func fixedPairCell(sink: FixedSink, source: FixedSource) -> some View {
        ZStack {
            Rectangle()
                .strokeBorder(Color.secondary.opacity(0.3), lineWidth: 0.5)
                .background(Rectangle().fill(Color.secondary.opacity(0.08)))
            Image(systemName: "lock")
                .font(.system(size: 9))
                .foregroundColor(.secondary)
        }
        .frame(width: cellSize, height: cellSize)
        .help("\(source.label) and \(sink.label) both answer nothing: neither end can be told where to send or where to listen, so this pairing is set on the units and read only here.")
    }

    private func helpText(sink: FixedSink, column: RoutingMatrix.Column,
                          feeding: NmosSender?, isThisSender: Bool) -> String {
        if isThisSender {
            return "\(column.label) feeds \(sink.label) at \(sink.multicastAddress):\(sink.port). Click to stop it; click another sender to move the feed."
        }
        if let feeding {
            return "\(sink.label) is fed by \(feeding.label). Clicking here moves the feed to \(column.label): one sender per destination, because two on one address is a collision."
        }
        return "Send \(column.label) to \(sink.label) at \(sink.multicastAddress):\(sink.port)"
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
            Text("\(controller.nodes.count) node\(controller.nodes.count == 1 ? "" : "s"), \(controller.matrix.columns.count) sender\(controller.matrix.columns.count == 1 ? "" : "s"), \(controller.matrix.rows.count) receiver\(controller.matrix.rows.count == 1 ? "" : "s")")
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
