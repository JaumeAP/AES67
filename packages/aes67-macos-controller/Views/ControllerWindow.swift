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

    /// The destinations that answer to nobody, as rows of the matrix.
    ///
    /// A PTP peer that follows a master is something being fed -- an
    /// amplifier, in a Dolby room -- and it is the only trace such a unit
    /// leaves on the network, because Atmos Connect announces nothing and has
    /// no control protocol. It cannot be told anything, so the crosspoint
    /// configures the source: our sender is re-addressed to where that unit
    /// already listens, which is its manual's address and port and not
    /// anything it reports.
    private var fixedSinks: [FixedSink] {
        discovery.ptpParticipants
            .filter { $0.role == "slave" }
            .map { peer in
                FixedSink(id: peer.clockId,
                          label: "PTP sink \(peer.clockId)",
                          multicastAddress: FixedSink.atmosConnectAddress,
                          port: FixedSink.atmosConnectPort,
                          note: "Follows a clock master and announces nothing: a unit configured "
                              + "by hand, which is what Dolby Atmos Connect is. A crosspoint here "
                              + "re-addresses the sender to \(FixedSink.atmosConnectAddress):"
                              + "\(FixedSink.atmosConnectPort), the factory default — change it on "
                              + "the unit's own side if this room uses another.")
            }
    }

    /// The sources that answer to nobody: a PTP master that announces nothing
    /// is a unit sending where its own configuration says -- a Dolby
    /// processor feeding a room. One of our receivers can be pointed at it,
    /// because that end can be told where to listen; a fixed sink cannot, and
    /// the matrix shows that pairing as read only.
    private var fixedSources: [FixedSource] {
        discovery.ptpParticipants
            .filter { $0.role == "master" }
            .map { peer in
                FixedSource(id: peer.clockId,
                            label: "PTP source \(peer.clockId)",
                            multicastAddress: FixedSink.atmosConnectAddress,
                            port: FixedSink.atmosConnectPort,
                            note: "A clock master that announces nothing: a unit configured by "
                                + "hand. Where it sends is set on the unit, so a receiver of ours "
                                + "is the end that can be told to listen there.")
            }
    }

    var body: some View {
        TabView {
            DeviceListView(nmos: nmos)
                .tabItem { Label("Devices", systemImage: "square.stack.3d.up") }
            sessionList
                .tabItem { Label("Sessions", systemImage: "antenna.radiowaves.left.and.right") }
            RoutingMatrixView(fixedSinks: fixedSinks, fixedSources: fixedSources)
                .tabItem { Label("Routing", systemImage: "square.grid.3x3") }
            ChannelGridView(nmos: nmos)
                .tabItem { Label("Channels", systemImage: "slider.horizontal.3") }
            LinkView(discovery: discovery)
                .tabItem { Label("Link", systemImage: "network") }
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
