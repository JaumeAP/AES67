//
// SessionList.swift
// AES67 Manager
//
// One list of the sessions on the network, whatever found them.
//
// The driver merges what it hears itself -- SAP announcements and
// `_rtsp._tcp` services it describes over RTSP -- into a single directory
// (NetworkEngine/Discovery/SessionDirectory.h). NMOS is discovered by this
// app instead, over IS-04, so the last merge happens here: an NMOS sender
// whose transport file names the same multicast destination as an announced
// session is that session, seen a third way, and showing it twice would
// invite subscribing to it twice.
//
// Pure values, no SwiftUI and no network, so run-tests.sh covers it.
//

import Foundation

/// How a session was found. A session can carry more than one: gear that
/// announces over SAP and also registers `_rtsp._tcp` is ordinary, and the
/// driver merges the two sightings into one entry rather than listing it
/// twice (NetworkEngine/Discovery/SessionDirectory.h). NMOS is added here,
/// because that discovery belongs to the app.
enum SessionDiscoveryRoute: String, Equatable {
    case sap
    case rtsp
    case nmos

    var label: String {
        switch self {
        case .sap:  return "SAP"
        case .rtsp: return "RTSP"
        case .nmos: return "NMOS"
        }
    }
}

/// One session as the driver's discovery gateway publishes it.
struct DiscoveredNetworkSession: Identifiable, Equatable {
    let sessionName: String
    let sourceAddress: String
    let multicastAddress: String
    let port: Int
    let ptpDomain: Int
    /// The sender's own SDP, complete — enough to add the stream without
    /// asking the user to retype anything.
    let sdp: String
    /// Every route the driver heard this session by, first heard first.
    let routes: [SessionDiscoveryRoute]

    /// Announcer + session name, which is what the driver dedupes on.
    var id: String { "\(sourceAddress)|\(sessionName)" }

    var routeLabel: String {
        routes.isEmpty ? "—" : routes.map(\.label).joined(separator: " + ")
    }
}

/// A session offered to the user, with every route it was found by.
struct UnifiedSession: Identifiable, Equatable {
    let name: String
    let sourceAddress: String
    let multicastAddress: String
    let port: Int
    let ptpDomain: Int
    /// The sender's own SDP. Empty only when a route knows of a session but
    /// has not handed over a description -- an NMOS sender whose transport
    /// file could not be read.
    let sdp: String
    let routes: [SessionDiscoveryRoute]
    /// IS-05 sender id, for a session that came from NMOS.
    let nmosSenderId: String?

    /// The destination is the identity here: two routes describing one flow
    /// agree on where the audio is, whatever they disagree on elsewhere.
    var id: String { "\(multicastAddress):\(port)" }

    var routeLabel: String {
        routes.isEmpty ? "—" : routes.map(\.label).joined(separator: " + ")
    }

    var canSubscribe: Bool { !multicastAddress.isEmpty && port > 0 }
}

/// An NMOS sender as this app knows it before merging: what IS-04 said, plus
/// the transport file IS-05 served for it.
struct NmosSessionCandidate: Equatable {
    let senderId: String
    let label: String
    let host: String
    let sdp: String
}

enum SessionList {
    /// Merges what the driver found with what NMOS offered.
    ///
    /// Keyed on the destination the audio is on, read from the SDP's `c=`
    /// and `m=audio` lines for an NMOS candidate. A candidate whose transport
    /// file names a destination already in the driver's list adds NMOS to
    /// that session's routes rather than a second row; one that names a
    /// destination nobody announced is a session of its own, which is the
    /// whole point -- a node behind a switch that filters SAP is reachable
    /// only this way.
    static func merge(driverSessions: [DiscoveredNetworkSession],
                      nmosCandidates: [NmosSessionCandidate]) -> [UnifiedSession] {
        var unified: [UnifiedSession] = driverSessions.map { session in
            UnifiedSession(name: session.sessionName,
                           sourceAddress: session.sourceAddress,
                           multicastAddress: session.multicastAddress,
                           port: session.port,
                           ptpDomain: session.ptpDomain,
                           sdp: session.sdp,
                           routes: session.routes,
                           nmosSenderId: nil)
        }

        for candidate in nmosCandidates {
            let destination = Self.destination(in: candidate.sdp)
            guard let address = destination.address, destination.port > 0 else {
                // No usable transport file: listed on its own so the node is
                // at least visible, and refused at subscribe time rather than
                // silently dropped here.
                unified.append(UnifiedSession(name: candidate.label,
                                              sourceAddress: candidate.host,
                                              multicastAddress: "",
                                              port: 0,
                                              ptpDomain: 0,
                                              sdp: candidate.sdp,
                                              routes: [.nmos],
                                              nmosSenderId: candidate.senderId))
                continue
            }

            if let index = unified.firstIndex(where: {
                $0.multicastAddress == address && $0.port == destination.port
            }) {
                let existing = unified[index]
                var routes = existing.routes
                if !routes.contains(.nmos) { routes.append(.nmos) }
                unified[index] = UnifiedSession(name: existing.name,
                                               sourceAddress: existing.sourceAddress,
                                               multicastAddress: existing.multicastAddress,
                                               port: existing.port,
                                               ptpDomain: existing.ptpDomain,
                                               // Keep the description already
                                               // held unless there was none.
                                               sdp: existing.sdp.isEmpty ? candidate.sdp : existing.sdp,
                                               routes: routes,
                                               nmosSenderId: candidate.senderId)
                continue
            }

            unified.append(UnifiedSession(name: candidate.label,
                                          sourceAddress: candidate.host,
                                          multicastAddress: address,
                                          port: destination.port,
                                          ptpDomain: 0,
                                          sdp: candidate.sdp,
                                          routes: [.nmos],
                                          nmosSenderId: candidate.senderId))
        }

        return unified.sorted { left, right in
            if left.name.lowercased() != right.name.lowercased() {
                return left.name.lowercased() < right.name.lowercased()
            }
            return left.id < right.id
        }
    }

    /// Where the audio of an SDP is: the `c=` address without its TTL suffix,
    /// and the port from `m=audio`. RFC 4566 §5.7 writes a multicast
    /// connection as <address>/<ttl>, and the TTL is not part of an address.
    static func destination(in sdp: String) -> (address: String?, port: Int) {
        var address: String?
        var port = 0

        // Character.isNewline rather than a comparison against "\n" and
        // "\r": Swift reads CRLF as ONE Character, so a line ending written
        // the way SDP writes it matches neither of them and the whole
        // description arrives as a single line.
        for rawLine in sdp.split(whereSeparator: { $0.isNewline }) {
            let line = rawLine.trimmingCharacters(in: .whitespaces)
            if line.hasPrefix("c=") {
                if let last = line.split(separator: " ").last {
                    address = last.split(separator: "/").first.map(String.init)
                }
            } else if line.hasPrefix("m=audio ") {
                let fields = line.dropFirst("m=audio ".count).split(separator: " ")
                if let first = fields.first, let parsed = Int(first.split(separator: "/").first ?? "") {
                    port = parsed
                }
            }
        }
        return (address, port)
    }
}
