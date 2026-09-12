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

@MainActor
final class DiscoveryService: ObservableObject {
    @Published private(set) var sessions: [DiscoveredNetworkSession] = []
    @Published private(set) var running = false
    @Published private(set) var lastError: String?

    private var handle: OpaquePointer?
    private var timer: Timer?

    /// Starts SAP and RTSP discovery. `interfaceIP` empty leaves the
    /// interface to the routing table, which is what a controller on a single
    /// network wants; naming it is for a machine with a separate audio LAN.
    func start(interfaceIP: String = "") {
        guard handle == nil else { return }

        handle = interfaceIP.withCString { pointer in
            aes67_discovery_start(interfaceIP.isEmpty ? nil : pointer, 1, 1)
        }
        guard handle != nil else {
            lastError = "Neither SAP nor mDNS discovery could start. Another process may hold "
                      + "the SAP port, or the system responder may be unavailable."
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

    deinit {
        // No MainActor hop in a deinit: the handle's own stop is thread-safe
        // and the timer holds a weak reference.
        if let handle {
            aes67_discovery_stop(handle)
        }
    }
}
