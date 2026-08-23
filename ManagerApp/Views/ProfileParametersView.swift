//
// ProfileParametersView.swift
// AES67 Manager
// Every parameter the active compatibility profile governs — PTP/clock
// master settings and connection settings alike — in one window, with the
// ones the profile pins shown as read-only rather than hidden.
//
// The point is that nothing is invisible: a parameter a profile fixes
// still appears, with its value and the reason it can't be changed, so
// selecting a profile never leaves the user wondering what it silently
// decided. Editable parameters keep working exactly as they do in the
// windows that already own them (AddStreamView's PTP domain stepper,
// PTPDiagnosticView's clock source, ContentView's channel selectors) —
// this window shows the settled result and the locks, it isn't a second
// place to configure the same things.
//

import SwiftUI

struct ProfileParametersView: View {
    @EnvironmentObject var driverManager: DriverManager

    private var profile: DriverManager.CompatibilityProfileOption {
        driverManager.activeCompatibilityProfile
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                header

                section("Synchronisation (PTP)") {
                    ptpRoleRow
                    ptpDomainRow
                    parameterRow(
                        "Clock source",
                        value: driverManager.ptpMasterCapable
                            ? (driverManager.ptpClockSourceKind == "internal"
                               ? "Internal (this Mac)" : "Locked to another audio device")
                            : "External (slave only)",
                        locked: profile.ptpRole != .any,
                        note: profile.ptpRole != .any
                            ? "Follows the PTP role \(profile.name) fixes."
                            : "Set in the PTP Diagnostics window."
                    )
                }

                section("Connection") {
                    parameterRow(
                        "Destination multicast address",
                        value: profile.recommendedMulticastAddress.isEmpty
                            ? "Per stream (no factory default documented)"
                            : "\(profile.recommendedMulticastAddress) (factory default)",
                        locked: false,
                        note: profile.requiredMulticastPrefix.isEmpty
                            ? "Set per stream when adding one. Factory defaults are a starting "
                              + "point — installations with more than one auditorium on a "
                              + "network give each its own address."
                            : "\(profile.name) requires addresses inside "
                              + "\(profile.requiredMulticastPrefix).0.0/16 — streams outside it "
                              + "are rejected."
                    )
                    flowAddressingRow
                    amplifierUnitRow
                    parameterRow(
                        "DSCP marking",
                        value: profile.recommendedDscp < 0
                            ? "None documented"
                            : "\(profile.recommendedDscp) (EF, factory default)",
                        locked: true,
                        note: "Informational only — this driver never sets a DSCP marking on "
                            + "its own traffic, whatever the profile documents."
                    )
                }

                section("Audio format") {
                    parameterRow(
                        "Sample rates",
                        value: profile.allowedSampleRates
                            .map { "\($0 / 1000) kHz" }
                            .joined(separator: ", "),
                        locked: true,
                        note: "Streams at any other rate are rejected under this profile."
                    )
                    parameterRow(
                        "Packet time",
                        value: profile.allowedPtimesMs.map { "\($0) ms" }.joined(separator: ", "),
                        locked: true,
                        note: "This driver's transmitter emits 1 ms packets and can't be "
                            + "reconfigured, so this is a hard limit either way."
                    )
                    parameterRow(
                        "Encoding",
                        value: profile.allowedEncodings.joined(separator: ", "),
                        locked: true,
                        note: "Chosen per stream from the encodings this profile accepts."
                    )
                    channelsRow
                }
            }
            .padding(24)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .frame(minWidth: 620, minHeight: 560)
    }

    // MARK: - Header

    private var header: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(profile.name)
                .font(.title2)
                .fontWeight(.semibold)
            Text(profile.caveats)
                .font(.callout)
                .foregroundColor(.secondary)
                .fixedSize(horizontal: false, vertical: true)
            if driverManager.isDriverLoaded {
                Label("The driver is installed — parameters read at startup only take effect "
                      + "the next time Core Audio starts it.",
                      systemImage: "info.circle")
                    .font(.caption)
                    .foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
    }

    // MARK: - Individual rows

    private var ptpRoleRow: some View {
        let forced = profile.ptpRole != .any
        let value: String = {
            switch profile.ptpRole {
            case .forcedMaster: return "Master (grandmaster)"
            case .forcedSlave:  return "Slave"
            case .any:          return driverManager.ptpMasterCapable
                                    ? "Master when BMCA elects us" : "Slave only"
            }
        }()
        return parameterRow(
            "PTP role",
            value: value,
            locked: forced,
            note: forced
                ? "\(profile.name) fixes this — the \"Act as PTP master\" toggle is locked to match."
                : "BMCA decides. Toggle it in the PTP Diagnostics window."
        )
    }

    private var ptpDomainRow: some View {
        parameterRow(
            "PTP domain",
            value: profile.domainIsFixed
                ? "\(profile.fixedDomain)"
                : (profile.recommendedPtpDomain >= 0
                   ? "\(profile.recommendedPtpDomain) (factory default)"
                   : "Set per stream"),
            locked: profile.domainIsFixed,
            note: profile.domainIsFixed
                ? "\(profile.name)'s mandatory configuration — streams on any other domain are rejected."
                : "Editable 0–127 when adding a stream, pre-filled with the factory default. "
                  + "Installations with several auditoriums on one network give each its own "
                  + "domain (109, 110, 111, …)."
        )
    }

    private var flowAddressingRow: some View {
        parameterRow(
            "Multi-flow addressing",
            value: profile.usesFixedMulticastPerFlowSourcePort
                ? "One address, source port per 8-channel flow"
                : "Address per 8-channel flow, shared port",
            locked: true,
            note: profile.usesFixedMulticastPerFlowSourcePort
                ? "Dolby Atmos Connect's own scheme: every flow shares the destination address "
                  + "and port, and is told apart by its source port instead."
                : "The AES67/Dante convention: each 8-channel flow gets the next multicast "
                  + "address, all on the same port."
        )
    }

    @ViewBuilder
    private var amplifierUnitRow: some View {
        let multiUnit = profile.maxUnits > 1
        let ports = driverManager.txSourcePorts(destinationPort: 6517)
        VStack(alignment: .leading, spacing: 4) {
            HStack(alignment: .firstTextBaseline) {
                Text("Amplifier unit")
                    .frame(width: 220, alignment: .leading)
                if multiUnit {
                    Picker("", selection: Binding(
                        get: { driverManager.amplifierUnit },
                        set: { driverManager.amplifierUnit = $0; driverManager.saveAmplifierUnit() }
                    )) {
                        ForEach(driverManager.amplifierUnitChoices, id: \.self) { unit in
                            Text("\(unit)").tag(unit)
                        }
                    }
                    .pickerStyle(.segmented)
                    .labelsHidden()
                    .frame(width: 140)
                    .disabled(driverManager.isDriverLoaded)
                } else {
                    Text("Single unit")
                        .foregroundColor(.secondary)
                    Image(systemName: "lock.fill")
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
                Spacer()
            }
            Text(multiUnit
                 ? "Up to \(profile.maxUnits) units chain in one auditorium, each carrying the "
                   + "next block of channels. Selecting a unit shifts this driver's flows to that "
                   + "unit's source ports"
                   + (ports.isEmpty ? "." : ": \(ports.map(String.init).joined(separator: ", ")) "
                      + "(with 6517 as the destination port).")
                 : "\(profile.name) is a single-unit profile — nothing to select.")
                .font(.caption)
                .foregroundColor(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    private var channelsRow: some View {
        parameterRow(
            "Channels",
            value: "\(driverManager.totalRxChannelCount) in / \(driverManager.totalTxChannelCount) out"
                + (profile.maxTotalChannels > 0 ? " (max \(profile.maxTotalChannels))" : ""),
            locked: false,
            note: {
                switch profile.direction {
                case .receiveOnly:
                    return "\(profile.name) is receive-only — the output selector is locked. "
                        + "Set the input count on the main window."
                case .transmitOnly:
                    return "\(profile.name) is transmit-only — the input selector is locked. "
                        + "Set the output count on the main window."
                case .any:
                    return "Set both counts on the main window."
                }
            }()
        )
    }

    // MARK: - Building blocks

    private func section<Content: View>(
        _ title: String,
        @ViewBuilder content: () -> Content
    ) -> some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(title)
                .font(.headline)
            content()
        }
        .padding(16)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Color.secondary.opacity(0.06))
        .cornerRadius(8)
    }

    private func parameterRow(
        _ name: String,
        value: String,
        locked: Bool,
        note: String
    ) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack(alignment: .firstTextBaseline) {
                Text(name)
                    .frame(width: 220, alignment: .leading)
                Text(value)
                    .foregroundColor(locked ? .secondary : .primary)
                if locked {
                    Image(systemName: "lock.fill")
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
                Spacer()
            }
            Text(note)
                .font(.caption)
                .foregroundColor(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
    }
}
