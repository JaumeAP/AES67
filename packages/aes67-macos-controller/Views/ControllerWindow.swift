//
// ControllerWindow.swift
// AES67 Controller
//
// Two things a person routing a plant needs open at once: what is on the
// network, and what is connected to what.
//

import SwiftUI

struct ControllerWindow: View {
    @StateObject private var discovery = DiscoveryService()
    @StateObject private var nmos = NmosController()

    /// One list, the way the Manager builds it: what SAP and RTSP found,
    /// with every NMOS sender merged in by the destination its transport file
    /// names (SessionList.merge, shared with that package).
    private var sessions: [UnifiedSession] {
        SessionList.merge(driverSessions: discovery.sessions,
                          nmosCandidates: nmos.sessionCandidates)
    }

    var body: some View {
        TabView {
            DeviceListView(nmos: nmos)
                .tabItem { Label("Devices", systemImage: "square.stack.3d.up") }
            sessionList
                .tabItem { Label("Sessions", systemImage: "antenna.radiowaves.left.and.right") }
            RoutingMatrixView()
                .tabItem { Label("Routing", systemImage: "square.grid.3x3") }
            ChannelGridView(nmos: nmos)
                .tabItem { Label("Channels", systemImage: "slider.horizontal.3") }
        }
        .frame(minWidth: 760, minHeight: 520)
        .onAppear {
            discovery.start()
            nmos.start()
        }
        .onDisappear {
            discovery.stop()
            nmos.stop()
        }
    }

    private var sessionList: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Sessions on this network")
                        .font(.title3)
                        .fontWeight(.semibold)
                    Text("Found over SAP, RTSP and NMOS by this application — no driver needed")
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
                Spacer()
                Button("Refresh") {
                    discovery.refresh()
                    nmos.refresh()
                }
            }
            .padding()

            Divider()

            if let error = discovery.lastError {
                notice(error, icon: "exclamationmark.triangle")
            } else if sessions.isEmpty {
                notice("Nothing found yet. Announcements repeat every 30 seconds or so, and a "
                     + "registered service is asked again periodically.",
                       icon: "antenna.radiowaves.left.and.right")
            } else {
                List(sessions) { session in
                    VStack(alignment: .leading, spacing: 3) {
                        Text(session.name.isEmpty ? "(unnamed session)" : session.name)
                        Text(detail(of: session))
                            .font(.caption)
                            .foregroundColor(.secondary)
                    }
                    .padding(.vertical, 3)
                }
            }
        }
    }

    private func detail(of session: UnifiedSession) -> String {
        let destination = session.canSubscribe
            ? "\(session.multicastAddress):\(session.port)"
            : "no transport file"
        return "\(destination)  ·  from \(session.sourceAddress)  ·  \(session.routeLabel)"
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
}
