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

// MARK: - Clocks and IS-08
//
// The two things a Dante Controller user looks for that device-level routing
// does not answer: which clock everything is following, and what feeds each
// output CHANNEL. IS-04 `self` carries the first, IS-08 the second.

func runNmosClockAndChannelMapTests() {
    // A node that is following a grandmaster says so, and the summary is what
    // a device list shows in its clock column.
    let selfWithPtp = """
    {"id": "n-1", "label": "Console", "clocks": [
      {"name": "clk0", "ref_type": "ptp", "traceable": true, "locked": true,
       "version": "IEEE1588-2008", "gmid": "00-1d-c1-ff-fe-12-34-56"}]}
    """.data(using: .utf8)!
    let clocks = (try? NmosDecoding.clocks(nodeSelf: selfWithPtp)) ?? []
    check(clocks.count == 1, "one clock decoded")
    check(clocks.first?.refType == "ptp", "a PTP reference")
    check(clocks.first?.locked == true, "locked")
    check(clocks.first?.summary == "Locked to 00-1d-c1-ff-fe-12-34-56", "the clock column's text")

    // A free-running node is not an error, and neither is one that declares
    // no clock at all.
    let internalClock = """
    {"id": "n-2", "clocks": [{"name": "clk0", "ref_type": "internal"}]}
    """.data(using: .utf8)!
    check((try? NmosDecoding.clocks(nodeSelf: internalClock))?.first?.summary == "Internal",
          "an internal clock says so")
    let noClock = "{\"id\": \"n-3\"}".data(using: .utf8)!
    check(((try? NmosDecoding.clocks(nodeSelf: noClock)) ?? []).isEmpty, "no clocks is empty, not a throw")

    // IS-08 is found the way IS-05 is: a control on a device, by URN.
    let devices = """
    [{"id": "d-1", "controls": [
       {"type": "urn:x-nmos:control:sr-ctrl/v1.1", "href": "http://host:8080/x-nmos/connection/v1.1"},
       {"type": "urn:x-nmos:control:cm-ctrl/v1.0", "href": "http://host:8080/x-nmos/channelmapping/v1.0"}]}]
    """.data(using: .utf8)!
    check((try? NmosDecoding.channelMappingRoot(devices: devices))??.absoluteString
          == "http://host:8080/x-nmos/channelmapping/v1.0/", "the IS-08 root, with its trailing slash")
    check((try? NmosDecoding.connectionRoot(devices: devices))??.absoluteString
          == "http://host:8080/x-nmos/connection/v1.1/", "and IS-05 is still found beside it")

    // The grid's axes.
    let io = """
    {"inputs": {"in-1": {"properties": {"name": "Stream 1"},
                          "channels": [{"label": "Left"}, {"label": "Right"}]}},
     "outputs": {"out-1": {"properties": {"name": "Device Out"},
                            "channels": [{"label": "Out 1"}, {"label": "Out 2"}]}}}
    """.data(using: .utf8)!
    let ports = try? NmosDecoding.channelMapIO(io)
    check(ports?.inputs.first?.label == "Stream 1", "an input port's name")
    check(ports?.inputs.first?.channels.count == 2, "with its channels")
    check(ports?.inputs.first?.channels.first?.label == "Left", "labelled as the device labels them")
    check(ports?.outputs.first?.channels.last?.id == "1", "channels are addressed by index")

    // And what is actually connected in it.
    let active = """
    {"map": {"out-1": {"0": {"input": "in-1", "channel_index": 1},
                        "1": {"input": null, "channel_index": null}}}}
    """.data(using: .utf8)!
    let map = NmosChannelMap(inputs: ports?.inputs ?? [], outputs: ports?.outputs ?? [],
                             active: (try? NmosDecoding.channelMapActive(active)) ?? [:])
    let source = map.source(ofOutput: "out-1", channel: 0)
    check(source?.input == "in-1", "the input feeding an output channel")
    check(source?.channel == 1, "and which of its channels")
    check(map.source(ofOutput: "out-1", channel: 1) == nil, "a muted output channel has no source")

    // The patch that changes one crosspoint, and the one that mutes it.
    let connect = NmosPatch.mapChannel(output: "out-1", outputChannel: 0,
                                       input: "in-1", inputChannel: 1)
    let connectBody = (try? JSONSerialization.jsonObject(with: connect)) as? [String: Any]
    let action = (connectBody?["action"] as? [String: Any])?["out-1"] as? [String: Any]
    let entry = action?["0"] as? [String: Any]
    check(entry?["input"] as? String == "in-1", "the patch names the input")
    check(entry?["channel_index"] as? Int == 1, "and its channel")
    check(((connectBody?["activation"] as? [String: Any])?["mode"] as? String) == "activate_immediate",
          "applied immediately, like IS-05")

    let mute = NmosPatch.mapChannel(output: "out-1", outputChannel: 0, input: nil, inputChannel: nil)
    let muteBody = (try? JSONSerialization.jsonObject(with: mute)) as? [String: Any]
    let muteAction = (muteBody?["action"] as? [String: Any])?["out-1"] as? [String: Any]
    let muteEntry = muteAction?["0"] as? [String: Any]
    check(muteEntry?["input"] is NSNull, "muting sends a null input")
}
