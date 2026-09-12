//
// NmosResources.swift
// AES67 Manager
//
// The pure half of the NMOS controller: what an IS-04 node says, what an
// IS-05 receiver holds, what a PATCH asks, and how a set of nodes becomes
// the routing matrix. No network here, so run-tests.sh can cover it.
//

import Foundation

struct NmosSender: Identifiable, Equatable {
    let id: String
    let label: String
    let nodeId: String
    /// From the sender's flow's source, when the chain is intact.
    let channels: Int?
}

struct NmosReceiver: Identifiable, Equatable {
    let id: String
    let label: String
    let nodeId: String
    /// IS-05 `active`: the truth about what this receiver is taking.
    var activeSenderId: String? = nil
    var masterEnable: Bool = false
}

/// A node's reference clock, as IS-04 `self` reports it: which clock it is,
/// whether it is traceable to a grandmaster, and which grandmaster. This is
/// the column Dante Controller calls "Clock Status" -- who is master, and
/// whether this device is locked to it.
struct NmosClock: Equatable {
    let name: String            // "clk0"
    let refType: String         // "internal", "ptp"
    let traceable: Bool
    let locked: Bool
    let grandmasterId: String   // empty when the node does not say
    let version: String         // "IEEE1588-2008"

    var summary: String {
        if refType == "internal" { return "Internal" }
        if !locked { return "Not locked" }
        if grandmasterId.isEmpty { return traceable ? "Locked, traceable" : "Locked" }
        return "Locked to \(grandmasterId)"
    }
}

/// One channel of an IS-08 input or output: what a grid row and column are.
struct NmosChannel: Identifiable, Equatable {
    let id: String              // "0", "1" — the index IS-08 addresses it by
    let label: String

    var index: Int { Int(id) ?? 0 }
}

/// An IS-08 input or output port, with its channels.
struct NmosIOPort: Identifiable, Equatable {
    let id: String
    let label: String
    let channels: [NmosChannel]
}

/// What IS-08 says a device is doing: for every output channel, which input
/// channel feeds it, or nothing. The grid Dante Controller draws, channel by
/// channel rather than device by device.
struct NmosChannelMap: Equatable {
    let inputs: [NmosIOPort]
    let outputs: [NmosIOPort]
    /// Keyed by "outputId/channelIndex", holding "inputId/channelIndex", or
    /// absent when that output channel is muted.
    let active: [String: String]

    static let empty = NmosChannelMap(inputs: [], outputs: [], active: [:])

    static func key(output: String, channel: Int) -> String { "\(output)/\(channel)" }
    static func value(input: String, channel: Int) -> String { "\(input)/\(channel)" }

    /// What feeds one output channel: the input port id and channel index, or
    /// nil for muted.
    func source(ofOutput output: String, channel: Int) -> (input: String, channel: Int)? {
        guard let value = active[Self.key(output: output, channel: channel)] else { return nil }
        let parts = value.split(separator: "/")
        guard parts.count == 2, let index = Int(parts[1]) else { return nil }
        return (String(parts[0]), index)
    }
}

struct NmosNode: Identifiable, Equatable {
    let id: String
    let label: String
    let host: String
    let port: Int
    /// The IS-05 root from the device's sr-ctrl control. nil: read-only.
    var connectionRoot: URL? = nil
    /// The IS-08 root from the device's cm-ctrl control. nil: this node maps
    /// no channels, which is most gear.
    var channelMappingRoot: URL? = nil
    var senders: [NmosSender] = []
    var receivers: [NmosReceiver] = []
    var clocks: [NmosClock] = []
    var reachable: Bool = true

    /// What to show in a device list's clock column.
    var clockSummary: String {
        guard let clock = clocks.first else { return "—" }
        return clock.summary
    }
}

enum NmosDecodingError: Error {
    case shape(String)
}

enum NmosDecoding {
    private static func object(_ data: Data) throws -> [String: Any] {
        guard let object = try JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            throw NmosDecodingError.shape("expected a JSON object")
        }
        return object
    }

    private static func array(_ data: Data) throws -> [[String: Any]] {
        guard let array = try JSONSerialization.jsonObject(with: data) as? [[String: Any]] else {
            throw NmosDecodingError.shape("expected a JSON array of objects")
        }
        return array
    }

    static func nodeSelf(_ data: Data) throws -> (id: String, label: String) {
        let node = try object(data)
        guard let id = node["id"] as? String else { throw NmosDecodingError.shape("self has no id") }
        return (id, node["label"] as? String ?? id)
    }

    /// The first sr-ctrl control of the first device that has one.
    static func connectionRoot(devices data: Data) throws -> URL? {
        try controlRoot(devices: data, urnPrefix: "urn:x-nmos:control:sr-ctrl/v1.")
    }

    /// The first cm-ctrl control: IS-08, the channel mapping a grid is drawn
    /// from. Absent on gear that routes whole streams and nothing finer.
    static func channelMappingRoot(devices data: Data) throws -> URL? {
        try controlRoot(devices: data, urnPrefix: "urn:x-nmos:control:cm-ctrl/v1.")
    }

    private static func controlRoot(devices data: Data, urnPrefix: String) throws -> URL? {
        for device in try array(data) {
            for control in device["controls"] as? [[String: Any]] ?? [] {
                guard let type = control["type"] as? String,
                      type.hasPrefix(urnPrefix),
                      let href = control["href"] as? String,
                      let url = URL(string: href.hasSuffix("/") ? href : href + "/") else { continue }
                return url
            }
        }
        return nil
    }

    /// The reference clocks a node declares in `self`. A node with none is
    /// not an error: it is a node that says nothing about its clock.
    static func clocks(nodeSelf data: Data) throws -> [NmosClock] {
        let node = try object(data)
        return (node["clocks"] as? [[String: Any]] ?? []).map { clock in
            NmosClock(name: clock["name"] as? String ?? "",
                      refType: clock["ref_type"] as? String ?? "internal",
                      traceable: clock["traceable"] as? Bool ?? false,
                      locked: clock["locked"] as? Bool ?? false,
                      grandmasterId: clock["gmid"] as? String ?? "",
                      version: clock["version"] as? String ?? "")
        }
    }

    /// IS-08 `io`: the ports and their channels, both directions.
    static func channelMapIO(_ data: Data) throws -> (inputs: [NmosIOPort], outputs: [NmosIOPort]) {
        let io = try object(data)
        func ports(_ key: String) -> [NmosIOPort] {
            let group = io[key] as? [String: Any] ?? [:]
            return group.compactMap { id, value -> NmosIOPort? in
                guard let port = value as? [String: Any] else { return nil }
                let channels = (port["channels"] as? [[String: Any]] ?? [])
                    .enumerated()
                    .map { index, channel in
                        NmosChannel(id: String(index),
                                    label: channel["label"] as? String ?? "Channel \(index + 1)")
                    }
                let properties = port["properties"] as? [String: Any] ?? [:]
                return NmosIOPort(id: id,
                                  label: properties["name"] as? String ?? id,
                                  channels: channels)
            }
            .sorted { $0.label.localizedStandardCompare($1.label) == .orderedAscending }
        }
        return (ports("inputs"), ports("outputs"))
    }

    /// IS-08 `map/active`: what feeds every output channel right now.
    static func channelMapActive(_ data: Data) throws -> [String: String] {
        let body = try object(data)
        let map = body["map"] as? [String: Any] ?? [:]
        var active: [String: String] = [:]
        for (outputId, value) in map {
            guard let channels = value as? [String: Any] else { continue }
            for (channelIndex, entry) in channels {
                guard let action = entry as? [String: Any],
                      let inputId = action["input"] as? String,
                      let inputChannel = action["channel_index"] as? Int,
                      let outputChannel = Int(channelIndex) else { continue }
                active[NmosChannelMap.key(output: outputId, channel: outputChannel)] =
                    NmosChannelMap.value(input: inputId, channel: inputChannel)
            }
        }
        return active
    }

    static func senders(_ senders: Data, flows: Data, sources: Data, nodeId: String) throws -> [NmosSender] {
        var sourceOfFlow: [String: String] = [:]
        for flow in try array(flows) {
            if let id = flow["id"] as? String, let source = flow["source_id"] as? String {
                sourceOfFlow[id] = source
            }
        }
        var channelsOfSource: [String: Int] = [:]
        for source in try array(sources) {
            if let id = source["id"] as? String, let channels = source["channels"] as? [Any] {
                channelsOfSource[id] = channels.count
            }
        }
        return try array(senders).map { sender in
            guard let id = sender["id"] as? String else { throw NmosDecodingError.shape("sender has no id") }
            var channels: Int? = nil
            if let flow = sender["flow_id"] as? String, let source = sourceOfFlow[flow] {
                channels = channelsOfSource[source]
            }
            return NmosSender(id: id, label: sender["label"] as? String ?? id, nodeId: nodeId, channels: channels)
        }
    }

    static func receivers(_ data: Data, nodeId: String) throws -> [NmosReceiver] {
        return try array(data).map { receiver in
            guard let id = receiver["id"] as? String else { throw NmosDecodingError.shape("receiver has no id") }
            return NmosReceiver(id: id, label: receiver["label"] as? String ?? id, nodeId: nodeId)
        }
    }

    static func active(_ data: Data) throws -> (senderId: String?, masterEnable: Bool) {
        let active = try object(data)
        return (active["sender_id"] as? String, active["master_enable"] as? Bool ?? false)
    }

    /// "error (debug)" from an IS-05 error body, or nil when it is not one.
    static func errorText(_ data: Data) -> String? {
        guard let body = try? object(data), let error = body["error"] as? String else { return nil }
        if let debug = body["debug"] as? String, !debug.isEmpty { return "\(error) (\(debug))" }
        return error
    }
}

enum NmosPatch {
    static func connect(senderId: String, sdp: String) -> Data {
        let body: [String: Any] = [
            "sender_id": senderId,
            "master_enable": true,
            "transport_file": ["data": sdp, "type": "application/sdp"],
            "activation": ["mode": "activate_immediate"],
        ]
        return try! JSONSerialization.data(withJSONObject: body)
    }

    /// One IS-08 crosspoint, applied immediately: an input channel onto an
    /// output channel, or nothing onto it, which is IS-08's way of muting.
    static func mapChannel(output: String, outputChannel: Int,
                           input: String?, inputChannel: Int?) -> Data {
        let action: [String: Any] = [
            "input": input as Any? ?? NSNull(),
            "channel_index": inputChannel as Any? ?? NSNull(),
        ]
        let body: [String: Any] = [
            "activation": ["mode": "activate_immediate"],
            "action": [output: [String(outputChannel): action]],
        ]
        return try! JSONSerialization.data(withJSONObject: body)
    }

    /// Where a sender should transmit. IS-05 transport parameters, applied
    /// immediately: the destination a device with no control protocol already
    /// listens on.
    static func sendTo(multicastAddress: String, port: Int) -> Data {
        let body: [String: Any] = [
            "master_enable": true,
            "transport_params": [["destination_ip": multicastAddress, "destination_port": port]],
            "activation": ["mode": "activate_immediate"],
        ]
        return try! JSONSerialization.data(withJSONObject: body)
    }

    static func disconnect() -> Data {
        let body: [String: Any] = [
            "sender_id": NSNull(),
            "master_enable": false,
            "activation": ["mode": "activate_immediate"],
        ]
        return try! JSONSerialization.data(withJSONObject: body)
    }
}

struct RoutingMatrix: Equatable {
    struct Column: Identifiable, Equatable {
        let id: String
        let nodeLabel: String
        let label: String
        let reachable: Bool
        let sender: NmosSender
    }

    struct Row: Identifiable, Equatable {
        let id: String
        let nodeLabel: String
        let label: String
        let reachable: Bool
        /// The node serves IS-05, so a click can do something.
        let writable: Bool
        let receiver: NmosReceiver
        let node: NmosNode
    }

    enum Cell: Equatable {
        case off
        case on
        /// Unreachable node on either axis, or a receiver nobody can patch.
        case unavailable
    }

    let columns: [Column]
    let rows: [Row]
    private let nodesById: [String: NmosNode]

    static func build(from nodes: [NmosNode]) -> RoutingMatrix {
        let byLabel: (String, String) -> Bool = {
            $0.localizedCaseInsensitiveCompare($1) == .orderedAscending
        }
        let sorted = nodes.sorted { byLabel($0.label, $1.label) }
        var columns: [Column] = []
        var rows: [Row] = []
        for node in sorted {
            for sender in node.senders.sorted(by: { byLabel($0.label, $1.label) }) {
                columns.append(Column(id: sender.id, nodeLabel: node.label, label: sender.label,
                                      reachable: node.reachable, sender: sender))
            }
            for receiver in node.receivers.sorted(by: { byLabel($0.label, $1.label) }) {
                rows.append(Row(id: receiver.id, nodeLabel: node.label, label: receiver.label,
                                reachable: node.reachable, writable: node.connectionRoot != nil,
                                receiver: receiver, node: node))
            }
        }
        // Node ids come off the network and nothing on the link guarantees
        // they are unique: two ravenna-announce instances left on the
        // default --host announce the same one. uniqueKeysWithValues traps
        // on that, which would take the whole app down over somebody
        // else's duplicate; the first one seen wins instead.
        return RoutingMatrix(columns: columns, rows: rows,
                             nodesById: Dictionary(nodes.map { ($0.id, $0) },
                                                   uniquingKeysWith: { first, _ in first }))
    }

    func cell(row: Row, column: Column) -> Cell {
        if !row.reachable || !column.reachable || !row.writable { return .unavailable }
        if row.receiver.masterEnable && row.receiver.activeSenderId == column.id { return .on }
        return .off
    }

    static func == (lhs: RoutingMatrix, rhs: RoutingMatrix) -> Bool {
        return lhs.columns == rhs.columns && lhs.rows == rhs.rows
    }
}
