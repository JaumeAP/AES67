//
// DiscoveryService.swift
// AES67 Controller
//
// The sessions on the network, found by this application rather than by a
// driver.
//
// The Manager reads its list from the running driver, which is right there:
// the driver is already listening, and the app is about that machine. A
// controller is about other people's devices and has to work on a machine
// where the driver was never installed -- so it runs the same two discoverers
// itself, through the C bridge over aes67_net
// (NetworkEngine/Discovery/DiscoveryBridge.h). Same SAPListener, same mDNS
// browse and RTSP DESCRIBE, same SessionDirectory merging them by the
// session's own identity; only the process is different.
//

import Foundation

/// A PTP participant on the link: a clock, not a session. This is how gear
/// that announces nothing else is seen at all -- Dolby Atmos Connect is
/// configured by hand end to end, and the only thing it puts on the network
/// unasked is its clock.
struct PTPParticipant: Identifiable, Equatable {
    let clockId: String
    let oui: String
    let role: String          // master, slave, mixed, unknown
    let sourceIp: String
    let domain: Int
    let messageCount: Int
    let secondsSinceLastSeen: Int

    var id: String { clockId }

    /// What the role means for a room: a master is something feeding this
    /// system, a slave something it feeds.
    var roleDescription: String {
        switch role {
        case "master": return "Clock master — a source"
        case "slave":  return "Follows a master — a sink"
        case "mixed":  return "Both: master and slave"
        default:       return "Seen, role unclear"
        }
    }
}

/// A service registered on the link, whoever made the device: RAVENNA's RTSP,
/// NMOS's node and registry, Dante's netaudio family. Browsing a registration
/// is not speaking a protocol, which is what makes a Dante device listable
/// here and not controllable.
struct LinkService: Identifiable, Equatable {
    let name: String
    let type: String
    let host: String
    let address: String
    let port: Int
    let secondsSinceLastSeen: Int

    var id: String { "\(type)/\(name)" }

    /// The ecosystem a service type belongs to, for a column that says what
    /// kind of gear this is without pretending to talk to it.
    var ecosystem: String {
        if type.hasPrefix("_netaudio") { return "Dante" }
        if type.hasPrefix("_nmos") { return "NMOS" }
        if type.hasPrefix("_ravenna") { return "RAVENNA" }
        if type.hasPrefix("_rtsp") { return "RTSP (RAVENNA/AES67)" }
        return type
    }
}

@MainActor
final class DiscoveryService: ObservableObject {
    @Published private(set) var sessions: [DiscoveredNetworkSession] = []
    @Published private(set) var ptpParticipants: [PTPParticipant] = []
    @Published private(set) var services: [LinkService] = []
    @Published private(set) var running = false
    @Published private(set) var lastError: String?

    private var handle: OpaquePointer?
    private var timer: Timer?

    /// Starts everything there is: SAP announcements, `_rtsp._tcp` described
    /// over RTSP, the PTP clocks on the link, and the service registrations of
    /// every ecosystem this world uses. A controller has no compatibility
    /// profile narrowing it -- it is not carrying audio, it is finding out
    /// what is there -- so it runs the lot.
    ///
    /// `interfaceIP` empty leaves the interface to the routing table, which is
    /// what a controller on a single network wants; naming it is for a machine
    /// with a separate audio LAN.
    func start(interfaceIP: String = "", interfaceName: String = "") {
        guard handle == nil else { return }

        handle = interfaceIP.withCString { ipPointer in
            interfaceName.withCString { namePointer in
                aes67_discovery_start(interfaceIP.isEmpty ? nil : ipPointer,
                                      interfaceName.isEmpty ? nil : namePointer,
                                      1, 1, 1, 1)
            }
        }
        guard handle != nil else {
            lastError = "No discovery could start at all. Another process may hold the SAP "
                      + "port, or the system responder may be unavailable."
            running = false
            return
        }

        lastError = nil
        running = true
        refresh()
        // Announcements repeat every 30 s or so and RTSP sessions are
        // re-described on their own schedule; a second is fast enough to feel
        // immediate and slow enough to cost nothing.
        timer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.refresh() }
        }
    }

    func stop() {
        timer?.invalidate()
        timer = nil
        if let handle {
            aes67_discovery_stop(handle)
        }
        handle = nil
        running = false
        sessions = []
        ptpParticipants = []
        services = []
    }

    func refresh() {
        guard let handle else { return }
        guard let raw = aes67_discovery_sessions_json(handle) else { return }
        defer { aes67_discovery_free_string(raw) }

        let json = String(cString: raw)
        guard let data = json.data(using: .utf8),
              let array = try? JSONSerialization.jsonObject(with: data) as? [[String: Any]] else {
            lastError = "The discovery bridge returned something that is not a session list."
            return
        }

        refreshParticipants()
        refreshServices()

        sessions = array.compactMap { entry in
            guard let name = entry["sessionName"] as? String else { return nil }
            let routes = (entry["sources"] as? [String] ?? [])
                .compactMap { SessionDiscoveryRoute(rawValue: $0) }
            return DiscoveredNetworkSession(
                sessionName: name,
                sourceAddress: entry["sourceAddress"] as? String ?? "",
                multicastAddress: entry["multicastAddress"] as? String ?? "",
                port: entry["port"] as? Int ?? 0,
                ptpDomain: entry["ptpDomain"] as? Int ?? 0,
                sdp: entry["sdp"] as? String ?? "",
                routes: routes)
        }
    }

    private func refreshParticipants() {
        guard let handle, let raw = aes67_discovery_ptp_peers_json(handle) else { return }
        defer { aes67_discovery_free_string(raw) }
        guard let data = String(cString: raw).data(using: .utf8),
              let array = try? JSONSerialization.jsonObject(with: data) as? [[String: Any]] else {
            return
        }
        ptpParticipants = array.compactMap { entry in
            guard let clockId = entry["clockId"] as? String else { return nil }
            return PTPParticipant(clockId: clockId,
                                  oui: entry["oui"] as? String ?? "",
                                  role: entry["role"] as? String ?? "unknown",
                                  sourceIp: entry["sourceIp"] as? String ?? "",
                                  domain: entry["domain"] as? Int ?? 0,
                                  messageCount: entry["messageCount"] as? Int ?? 0,
                                  secondsSinceLastSeen: entry["secondsSinceLastSeen"] as? Int ?? 0)
        }
        .sorted { $0.clockId < $1.clockId }
    }

    private func refreshServices() {
        guard let handle, let raw = aes67_discovery_services_json(handle) else { return }
        defer { aes67_discovery_free_string(raw) }
        guard let data = String(cString: raw).data(using: .utf8),
              let array = try? JSONSerialization.jsonObject(with: data) as? [[String: Any]] else {
            return
        }
        services = array.compactMap { entry in
            guard let name = entry["name"] as? String, let type = entry["type"] as? String else {
                return nil
            }
            return LinkService(name: name,
                               type: type,
                               host: entry["host"] as? String ?? "",
                               address: entry["address"] as? String ?? "",
                               port: entry["port"] as? Int ?? 0,
                               secondsSinceLastSeen: entry["secondsSinceLastSeen"] as? Int ?? 0)
        }
        .sorted { ($0.ecosystem, $0.name) < ($1.ecosystem, $1.name) }
    }

    deinit {
        // No MainActor hop in a deinit: the handle's own stop is thread-safe
        // and the timer holds a weak reference.
        if let handle {
            aes67_discovery_stop(handle)
        }
    }
}
