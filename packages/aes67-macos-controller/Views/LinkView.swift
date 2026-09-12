//
// LinkView.swift
// AES67 Controller
//
// Everything on the segment, whoever made it and however it makes itself
// known.
//
// The other tabs are about what can be routed. This one is about what is
// there, which is not the same question and is the one asked first when a
// room does not work. Three ways of being seen, none of which needs the
// device's cooperation beyond what it already broadcasts:
//
//  - a registered service: RAVENNA's `_rtsp._tcp`, NMOS's node and registry,
//    Dante's `_netaudio-*._udp` family. Reading a registration is not
//    speaking a protocol, so a Dante device is listed here and controlled
//    nowhere -- which is the honest position, not a limitation to hide.
//  - a PTP clock: gear configured entirely by hand announces nothing at all,
//    and Dolby Atmos Connect is exactly that. Its clock is the one thing it
//    puts on the wire unasked.
//  - an announced or described session, which is the Sessions tab.
//

import SwiftUI

struct LinkView: View {
    @ObservedObject var discovery: DiscoveryService

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            header
            Divider()
            if discovery.services.isEmpty && discovery.ptpParticipants.isEmpty {
                notice("Nothing seen yet. Devices register their services and announce their "
                     + "clocks on their own schedule; a quiet segment takes a few seconds.")
            } else {
                List {
                    if !discovery.services.isEmpty {
                        Section("Registered services") {
                            ForEach(discovery.services) { service in
                                VStack(alignment: .leading, spacing: 2) {
                                    Text("\(service.name)  ·  \(service.ecosystem)")
                                    Text(detail(of: service))
                                        .font(.caption)
                                        .foregroundColor(.secondary)
                                }
                                .padding(.vertical, 2)
                            }
                        }
                    }
                    if !discovery.ptpParticipants.isEmpty {
                        Section("PTP clocks") {
                            ForEach(discovery.ptpParticipants) { peer in
                                VStack(alignment: .leading, spacing: 2) {
                                    Text("\(peer.clockId)  ·  \(peer.roleDescription)")
                                    Text("OUI \(peer.oui)  ·  from \(peer.sourceIp)  ·  domain "
                                       + "\(peer.domain)  ·  \(peer.messageCount) messages  ·  "
                                       + "seen \(peer.secondsSinceLastSeen)s ago")
                                        .font(.caption)
                                        .foregroundColor(.secondary)
                                }
                                .padding(.vertical, 2)
                            }
                        }
                    }
                }
            }
        }
    }

    private func detail(of service: LinkService) -> String {
        let where_ = service.address.isEmpty
            ? service.host
            : "\(service.address):\(service.port)"
        return "\(service.type)  ·  \(where_)  ·  seen \(service.secondsSinceLastSeen)s ago"
    }

    private var header: some View {
        HStack {
            VStack(alignment: .leading, spacing: 2) {
                Text("Link")
                    .font(.title3)
                    .fontWeight(.semibold)
                Text("Every device this segment shows, by service registration or by its clock — "
                   + "including gear nothing here can control")
                    .font(.caption)
                    .foregroundColor(.secondary)
            }
            Spacer()
            Button("Refresh") { discovery.refresh() }
        }
        .padding()
    }

    private func notice(_ text: String) -> some View {
        Text(text)
            .font(.callout)
            .foregroundColor(.secondary)
            .multilineTextAlignment(.center)
            .fixedSize(horizontal: false, vertical: true)
            .padding(40)
            .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}
