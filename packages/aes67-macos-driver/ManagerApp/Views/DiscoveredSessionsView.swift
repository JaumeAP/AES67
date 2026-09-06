//
// DiscoveredSessionsView.swift
// AES67 Manager
// Sessions other devices are announcing over SAP, ready to add without
// typing a multicast address by hand.
//
// The list comes from the running driver (DriverManager's SAP discovery
// gateway), which sweeps sessions that have stopped being announced — so
// what's shown is what's actually on the network right now, not a
// historical pile. Nothing here announces anything: this driver listens
// only.
//

import SwiftUI

struct DiscoveredSessionsView: View {
    @EnvironmentObject var driverManager: DriverManager
    @Environment(\.dismiss) var dismiss

    /// Sessions already added as streams, so the list can say so instead of
    /// letting the user add the same thing twice and get a rejection.
    private func isAlreadyAdded(_ session: DriverManager.DiscoveredSession) -> Bool {
        // Written out with explicit types rather than as a one-line `contains`
        // closure: the compact form mixed `Int(_:)` — which has a large
        // overload set — with `==` and `&&` inside a closure whose parameter
        // type had to be inferred, and swiftc gave up type-checking it in
        // reasonable time (the error that failed every CI run before the build
        // moved to this machine). Splitting it also surfaced what the timeout was
        // hiding: the closure read `$0.multicast.ip` and `$0.multicast.port`,
        // and `StreamInfo` has no `multicast` member at all — the fields are
        // `multicastIP` and `port`. The comparison never worked; it only ever
        // failed slowly enough to look like a compiler limit.
        let sessionAddress: String = session.multicastAddress
        let sessionPort: Int = session.port
        for stream in driverManager.streams {
            let streamAddress: String = stream.multicastIP
            let streamPort: Int = Int(stream.port)
            if streamAddress == sessionAddress && streamPort == sessionPort {
                return true
            }
        }
        return false
    }

    private var canReceive: Bool {
        driverManager.activeCompatibilityProfile.direction != .transmitOnly
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            header
            Divider()
            content
            Divider()
            footer
        }
        .frame(minWidth: 560, minHeight: 420)
        .onAppear { driverManager.refreshDiscoveredSessions() }
        // While this window is open, keep it current. SAP announcers repeat
        // roughly every 30 s, so anything faster than this just burns
        // queries for the same answer.
        .onReceive(Timer.publish(every: 5, on: .main, in: .common).autoconnect()) { _ in
            driverManager.refreshDiscoveredSessions()
        }
    }

    private var header: some View {
        HStack {
            VStack(alignment: .leading, spacing: 2) {
                Text("Discovered Sessions")
                    .font(.title3)
                    .fontWeight(.semibold)
                Text("Announced over SAP by other devices on this network")
                    .font(.caption)
                    .foregroundColor(.secondary)
            }
            Spacer()
            Button("Done") { dismiss() }
        }
        .padding()
    }

    @ViewBuilder
    private var content: some View {
        if !driverManager.isDriverInstalled {
            notice("The driver isn't installed, so nothing is listening for announcements. "
                 + "Turn it on from the main window.",
                   icon: "power")
        } else if !canReceive {
            notice("\(driverManager.activeCompatibilityProfile.name) is transmit-only — this "
                 + "driver can't receive, so discovered sessions can't be added under it.",
                   icon: "lock.fill")
        } else if driverManager.discoveredSessions.isEmpty {
            notice("Nothing announced yet. Devices repeat their announcements every 30 seconds "
                 + "or so, and a session disappears here once it stops being announced.",
                   icon: "antenna.radiowaves.left.and.right")
        } else {
            List(driverManager.discoveredSessions) { session in
                row(for: session)
            }
        }
    }

    private func row(for session: DriverManager.DiscoveredSession) -> some View {
        let added = isAlreadyAdded(session)
        return HStack(alignment: .top) {
            VStack(alignment: .leading, spacing: 3) {
                Text(session.sessionName.isEmpty ? "(unnamed session)" : session.sessionName)
                    .font(.body)
                Text("\(session.multicastAddress):\(session.port)  ·  from \(session.sourceAddress)"
                     + "  ·  PTP domain \(session.ptpDomain)")
                    .font(.caption)
                    .foregroundColor(.secondary)
            }
            Spacer()
            if added {
                Label("Added", systemImage: "checkmark.circle.fill")
                    .font(.caption)
                    .foregroundColor(.secondary)
            } else {
                Button("Add") { add(session) }
            }
        }
        .padding(.vertical, 4)
    }

    private func notice(_ text: String, icon: String) -> some View {
        VStack(spacing: 10) {
            Image(systemName: icon)
                .font(.system(size: 32))
                .foregroundColor(.secondary)
            Text(text)
                .font(.callout)
                .foregroundColor(.secondary)
                .multilineTextAlignment(.center)
                .fixedSize(horizontal: false, vertical: true)
        }
        .padding(40)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private var footer: some View {
        HStack {
            Text(driverManager.discoveredSessions.isEmpty
                 ? ""
                 : "\(driverManager.discoveredSessions.count) session"
                   + (driverManager.discoveredSessions.count == 1 ? "" : "s"))
                .font(.caption)
                .foregroundColor(.secondary)
            Spacer()
            Button("Refresh") { driverManager.refreshDiscoveredSessions() }
        }
        .padding()
    }

    /// Adds the session as a receive stream. Parameters come from the
    /// announcer's own SDP where it gave them, and fall back to this
    /// driver's own defaults where it didn't — the driver validates the
    /// result against the active profile either way, so a session that
    /// doesn't fit is refused with a reason rather than silently mangled.
    private func add(_ session: DriverManager.DiscoveredSession) {
        let sdp = session.sdp
        let channels = Self.intValue(in: sdp, after: "L24/48000/") ?? Self.channelsFromRtpmap(sdp) ?? 8
        let rate = Self.sampleRateFromRtpmap(sdp) ?? UInt32(driverManager.currentDeviceSampleRate)
        let encoding = sdp.contains("L16") ? "L16" : "L24"

        driverManager.addStream(
            name: session.sessionName.isEmpty ? "Discovered session" : session.sessionName,
            multicastIP: session.multicastAddress,
            port: UInt16(session.port),
            numChannels: UInt16(channels),
            sampleRate: rate,
            encoding: encoding,
            ptpDomain: session.ptpDomain,
            description: "Discovered over SAP from \(session.sourceAddress)"
        )
    }

    // MARK: - Minimal SDP field extraction
    //
    // Only what's needed to fill in the Add form. The driver does the real
    // parsing (Driver/SDPParser) when the stream is created; these are
    // best-effort reads of the announcer's rtpmap line, with defaults
    // behind them.

    /// "a=rtpmap:98 L24/48000/8" -> 8
    private static func channelsFromRtpmap(_ sdp: String) -> Int? {
        guard let line = sdp.split(separator: "\n").first(where: { $0.contains("rtpmap") }) else {
            return nil
        }
        let parts = line.split(separator: "/")
        guard parts.count >= 3 else { return nil }
        return Int(parts[2].trimmingCharacters(in: .whitespacesAndNewlines))
    }

    /// "a=rtpmap:98 L24/48000/8" -> 48000
    private static func sampleRateFromRtpmap(_ sdp: String) -> UInt32? {
        guard let line = sdp.split(separator: "\n").first(where: { $0.contains("rtpmap") }) else {
            return nil
        }
        let parts = line.split(separator: "/")
        guard parts.count >= 2 else { return nil }
        return UInt32(parts[1].trimmingCharacters(in: .whitespacesAndNewlines))
    }

    private static func intValue(in sdp: String, after marker: String) -> Int? {
        guard let range = sdp.range(of: marker) else { return nil }
        let rest = sdp[range.upperBound...].prefix(while: { $0.isNumber })
        return Int(rest)
    }
}
