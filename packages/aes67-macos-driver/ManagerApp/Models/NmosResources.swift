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

struct NmosNode: Identifiable, Equatable {
    let id: String
    let label: String
    let host: String
    let port: Int
    /// The IS-05 root from the device's sr-ctrl control. nil: read-only.
    var connectionRoot: URL? = nil
    var senders: [NmosSender] = []
    var receivers: [NmosReceiver] = []
    var reachable: Bool = true

    var nodeRoot: URL { URL(string: "http://\(host):\(port)/x-nmos/node/v1.3/")! }
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
        for device in try array(data) {
            for control in device["controls"] as? [[String: Any]] ?? [] {
                guard let type = control["type"] as? String,
                      type.hasPrefix("urn:x-nmos:control:sr-ctrl/v1."),
                      let href = control["href"] as? String,
                      let url = URL(string: href.hasSuffix("/") ? href : href + "/") else { continue }
                return url
            }
        }
        return nil
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
        return RoutingMatrix(columns: columns, rows: rows,
                             nodesById: Dictionary(uniqueKeysWithValues: nodes.map { ($0.id, $0) }))
    }

    func cell(row: Row, column: Column) -> Cell {
        if !row.reachable || !column.reachable || !row.writable { return .unavailable }
        if row.receiver.masterEnable && row.receiver.activeSenderId == column.id { return .on }
        return .off
    }

    func node(forSender id: String) -> NmosNode? {
        return nodesById[columns.first { $0.id == id }?.sender.nodeId ?? ""]
    }

    static func == (lhs: RoutingMatrix, rhs: RoutingMatrix) -> Bool {
        return lhs.columns == rhs.columns && lhs.rows == rhs.rows
    }
}
