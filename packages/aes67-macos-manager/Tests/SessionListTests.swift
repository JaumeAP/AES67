//
// SessionListTests.swift
// AES67 Manager
//
// The last merge: NMOS senders against what the driver already found.
//

import Foundation

private func driverSession(name: String, address: String, port: Int,
                           routes: [SessionDiscoveryRoute],
                           sdp: String = "") -> DiscoveredNetworkSession {
    DiscoveredNetworkSession(sessionName: name,
                                    sourceAddress: "192.168.1.50",
                                    multicastAddress: address,
                                    port: port,
                                    ptpDomain: 0,
                                    sdp: sdp,
                                    routes: routes)
}

private func transportFile(address: String, port: Int) -> String {
    """
    v=0\r
    o=- 42 1 IN IP4 192.168.1.90\r
    s=NMOS Sender\r
    c=IN IP4 \(address)/32\r
    t=0 0\r
    m=audio \(port) RTP/AVP 97\r
    a=rtpmap:97 L24/48000/8\r
    """
}

func runSessionListTests() {
    // The destination is read the way the driver reads it: the TTL is not
    // part of the address.
    let destination = SessionList.destination(in: transportFile(address: "239.69.0.1", port: 5004))
    check(destination.address == "239.69.0.1", "c= address without its TTL")
    check(destination.port == 5004, "m=audio port")

    // An NMOS sender pointing at a session already announced adds a route
    // rather than a row.
    let announced = driverSession(name: "Studio Mic 1", address: "239.69.0.1", port: 5004,
                                  routes: [.sap, .rtsp])
    let sameFlow = NmosSessionCandidate(senderId: "s-1", label: "Studio Mic 1",
                                        host: "node.local",
                                        sdp: transportFile(address: "239.69.0.1", port: 5004))
    let merged = SessionList.merge(driverSessions: [announced], nmosCandidates: [sameFlow])
    check(merged.count == 1, "one flow stays one row")
    check(merged.first?.routes == [.sap, .rtsp, .nmos], "every route it was heard by")
    check(merged.first?.routeLabel == "SAP + RTSP + NMOS", "routes shown as found")
    check(merged.first?.nmosSenderId == "s-1", "the IS-05 sender is remembered")

    // A sender nobody announced is a session of its own: that is the case
    // this merge exists for.
    let unannounced = NmosSessionCandidate(senderId: "s-2", label: "Booth Feed",
                                           host: "other.local",
                                           sdp: transportFile(address: "239.69.0.9", port: 5006))
    let both = SessionList.merge(driverSessions: [announced], nmosCandidates: [sameFlow, unannounced])
    check(both.count == 2, "an unannounced sender is listed too")
    check(both.contains { $0.multicastAddress == "239.69.0.9" && $0.routes == [.nmos] },
          "and it is marked as NMOS only")

    // Same destination, different ports: two flows, not one.
    let otherPort = NmosSessionCandidate(senderId: "s-3", label: "Studio Mic 1",
                                         host: "node.local",
                                         sdp: transportFile(address: "239.69.0.1", port: 5008))
    let ports = SessionList.merge(driverSessions: [announced], nmosCandidates: [otherPort])
    check(ports.count == 2, "the port is part of the identity")

    // A sender whose transport file says nothing usable is still visible,
    // and says it cannot be subscribed to.
    let broken = NmosSessionCandidate(senderId: "s-4", label: "Broken", host: "bad.local", sdp: "")
    let withBroken = SessionList.merge(driverSessions: [], nmosCandidates: [broken])
    check(withBroken.count == 1, "a sender with no transport file is still listed")
    check(withBroken.first?.canSubscribe == false, "and cannot be subscribed to")

    // An empty network is an empty list, not a crash.
    check(SessionList.merge(driverSessions: [], nmosCandidates: []).isEmpty, "nothing found, nothing shown")

    // Routes carry through from the driver untouched when NMOS says nothing.
    let driverOnly = SessionList.merge(driverSessions: [announced], nmosCandidates: [])
    check(driverOnly.first?.routes == [.sap, .rtsp], "the driver's own routes are kept")
}
