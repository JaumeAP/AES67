//
// NmosResourcesTests.swift
// AES67 Manager
//
// The pure half of the NMOS controller: what IS-04 and IS-05 JSON means,
// what a PATCH says, and how nodes become a matrix. No sockets.
//

import Foundation

private let senders = """
[{"id":"s1","label":"Mix A","flow_id":"f1","device_id":"d1"},
 {"id":"s2","label":"Mix B","flow_id":"f2","device_id":"d1"}]
""".data(using: .utf8)!
private let flows = """
[{"id":"f1","source_id":"src1"},{"id":"f2","source_id":"missing"}]
""".data(using: .utf8)!
private let sources = """
[{"id":"src1","channels":[{"label":"L"},{"label":"R"}]}]
""".data(using: .utf8)!
private let receivers = """
[{"id":"r1","label":"Return 1","device_id":"d1","subscription":{"sender_id":null,"active":false}}]
""".data(using: .utf8)!
private let devices = """
[{"id":"d1","label":"Dev","controls":[
  {"href":"http://10.0.0.5:8080/x-nmos/connection/v1.1/","type":"urn:x-nmos:control:sr-ctrl/v1.1"}]}]
""".data(using: .utf8)!
private let devicesNoControl = """
[{"id":"d1","label":"Dev","controls":[]}]
""".data(using: .utf8)!
private let selfBody = """
{"id":"n1","label":"Studio Mac","hostname":"studio-mac"}
""".data(using: .utf8)!

/// Every check in this file. Called from main.swift.
func runNmosResourcesTests() {
    // Decoding
    do {
        let node = try NmosDecoding.nodeSelf(selfBody)
        checkEqual(node.id, "n1", "self id")
        checkEqual(node.label, "Studio Mac", "self label")

        let root = try NmosDecoding.connectionRoot(devices: devices)
        checkEqual(root?.absoluteString, "http://10.0.0.5:8080/x-nmos/connection/v1.1/", "IS-05 root")
        check(try NmosDecoding.connectionRoot(devices: devicesNoControl) == nil, "no control means nil root")

        let decodedSenders = try NmosDecoding.senders(senders, flows: flows, sources: sources, nodeId: "n1")
        checkEqual(String(decodedSenders.count), "2", "two senders")
        checkEqual(decodedSenders[0].label, "Mix A", "sender label")
        check(decodedSenders[0].channels == 2, "channels through flow and source")
        check(decodedSenders[1].channels == nil, "a broken source link is no count")
        checkEqual(decodedSenders[0].nodeId, "n1", "sender carries its node")

        let decodedReceivers = try NmosDecoding.receivers(receivers, nodeId: "n1")
        checkEqual(decodedReceivers.first?.label, "Return 1", "receiver label")
        check(decodedReceivers.first?.activeSenderId == nil, "receiver starts with no sender")

        let active = try NmosDecoding.active("""
            {"sender_id":"s1","master_enable":true,"activation":{"mode":null}}
            """.data(using: .utf8)!)
        checkEqual(active.senderId, "s1", "active sender")
        check(active.masterEnable, "active master_enable")
        let idle = try NmosDecoding.active("""
            {"sender_id":null,"master_enable":false}
            """.data(using: .utf8)!)
        check(idle.senderId == nil && !idle.masterEnable, "idle receiver")
    } catch {
        check(false, "decoding threw: \(error)")
    }
    check((try? NmosDecoding.nodeSelf("[]".data(using: .utf8)!)) == nil, "wrong shape throws")
    checkEqual(NmosDecoding.errorText("""
        {"code":400,"error":"no room","debug":"needs 8 channels"}
        """.data(using: .utf8)!), "no room (needs 8 channels)", "error text with debug")
    checkEqual(NmosDecoding.errorText("""
        {"code":500,"error":"refused","debug":null}
        """.data(using: .utf8)!), "refused", "error text without debug")

    // PATCH bodies
    let connect = try! JSONSerialization.jsonObject(with: NmosPatch.connect(senderId: "s1", sdp: "v=0\r\n")) as! [String: Any]
    checkEqual(connect["sender_id"] as? String, "s1", "connect sender_id")
    check(connect["master_enable"] as? Bool == true, "connect master_enable")
    let file = connect["transport_file"] as? [String: Any]
    checkEqual(file?["data"] as? String, "v=0\r\n", "connect transport_file data")
    checkEqual(file?["type"] as? String, "application/sdp", "connect transport_file type")
    checkEqual((connect["activation"] as? [String: Any])?["mode"] as? String, "activate_immediate", "connect activation")

    let disconnect = try! JSONSerialization.jsonObject(with: NmosPatch.disconnect()) as! [String: Any]
    check(disconnect["master_enable"] as? Bool == false, "disconnect master_enable")
    check(disconnect["sender_id"] is NSNull, "disconnect sender_id is null")
    checkEqual((disconnect["activation"] as? [String: Any])?["mode"] as? String, "activate_immediate", "disconnect activation")

    // Matrix
    var mac = NmosNode(id: "n1", label: "Studio Mac", host: "10.0.0.5", port: 8080,
                       connectionRoot: URL(string: "http://10.0.0.5:8080/x-nmos/connection/v1.1/"))
    mac.senders = [NmosSender(id: "s1", label: "Mix A", nodeId: "n1", channels: 2)]
    mac.receivers = [NmosReceiver(id: "r1", label: "Return 1", nodeId: "n1", activeSenderId: "c1", masterEnable: true)]
    var card = NmosNode(id: "n2", label: "card", host: "10.0.0.6", port: 80, connectionRoot: nil)
    card.senders = [NmosSender(id: "c1", label: "Out", nodeId: "n2", channels: nil)]
    card.receivers = [NmosReceiver(id: "cr", label: "In", nodeId: "n2")]

    let matrix = RoutingMatrix.build(from: [mac, card])
    checkEqual(matrix.columns.map { $0.label }.joined(separator: ","), "Out,Mix A", "columns sorted by node label, case-insensitive")
    checkEqual(matrix.rows.map { $0.label }.joined(separator: ","), "In,Return 1", "rows sorted the same way")
    let macRow = matrix.rows[1], cardRow = matrix.rows[0]
    let outColumn = matrix.columns[0], mixColumn = matrix.columns[1]
    check(matrix.cell(row: macRow, column: outColumn) == .on, "active sender is on")
    check(matrix.cell(row: macRow, column: mixColumn) == .off, "other sender is off")
    check(matrix.cell(row: cardRow, column: mixColumn) == .unavailable, "a node without IS-05 is not writable")
    check(!cardRow.writable, "writable follows connectionRoot")

    // Two nodes with the same id. Node ids come off the network, and two
    // ravenna-announce instances left on the default --host announce the
    // same one, so a uniqueKeysWithValues here takes the whole app down
    // over somebody else's duplicate.
    var twin = NmosNode(id: "n1", label: "Twin", host: "10.0.0.7", port: 80, connectionRoot: nil)
    twin.senders = [NmosSender(id: "t1", label: "Twin Out", nodeId: "n1", channels: nil)]
    twin.receivers = [NmosReceiver(id: "tr", label: "Twin In", nodeId: "n1")]
    let duplicated = RoutingMatrix.build(from: [mac, twin])
    check(duplicated.columns.contains { $0.id == "s1" }, "the first of two nodes sharing an id keeps its column")
    check(duplicated.columns.contains { $0.id == "t1" }, "the second keeps its column too")
    check(duplicated.rows.contains { $0.id == "r1" }, "the first keeps its row")
    check(duplicated.rows.contains { $0.id == "tr" }, "the second keeps its row")

    card.reachable = false
    let withDown = RoutingMatrix.build(from: [mac, card])
    check(withDown.cell(row: withDown.rows[1], column: withDown.columns[0]) == .unavailable, "an unreachable sender node greys the column")
    check(!withDown.columns[0].reachable, "column carries reachability")
}
