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
        VStack(alignment: .leading, spacing: 0) {
            header
                .padding(24)

            Divider()

            TabView {
                // Master first: what governs the whole link, and what the
                // other two tabs inherit from.
                tab { masterContent }
                    .tabItem { Label("Master", systemImage: "clock") }

                // Input before output, so the tabs read in signal order for
                // this driver: what arrives, then what leaves.
                tab { inputsContent }
                    .tabItem { Label("Inputs", systemImage: "arrow.down.circle") }

                tab { outputsContent }
                    .tabItem { Label("Outputs", systemImage: "arrow.up.circle") }
            }
            .padding(.horizontal, 12)
            .padding(.bottom, 12)
        }
        .frame(minWidth: 660, minHeight: 600)
    }

    /// One tab's scrolling body — every tab has the same padding and
    /// alignment, so the frame doesn't jump when switching between them.
    private func tab<Content: View>(@ViewBuilder content: () -> Content) -> some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                content()
            }
            .padding(20)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .onAppear { driverManager.refreshPtpPeers() }
    }

    /// The list of Dolby elements found on the network by passive PTP
    /// observation, filtered to one side. Read-only for now — the next stage
    /// turns this list into the driver's own inputs/outputs.
    @ViewBuilder
    private func foundElementsSection(_ peers: [DriverManager.DiscoveredPeer],
                                      direction: DolbyIoDirection,
                                      side: String) -> some View {
        let resolved = direction == .input
            ? driverManager.resolvedInputChannels
            : driverManager.resolvedOutputChannels
        section("Detected elements (PTP) — \(side)") {
            if peers.isEmpty {
                Text("None detected yet. Elements are discovered passively from PTP "
                     + "traffic — a Dolby unit appears here once it is on the network "
                     + "and exchanging PTP with this driver. Each chained DMA unit "
                     + "shows as its own entry.")
                    .font(.caption).foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            } else {
                ForEach(peers) { peer in
                    VStack(alignment: .leading, spacing: 4) {
                        HStack(alignment: .firstTextBaseline) {
                            Image(systemName: "dot.radiowaves.left.and.right")
                                .font(.caption).foregroundColor(.secondary)
                            Text(peer.sourceIp.isEmpty ? peer.clockId : peer.sourceIp)
                                .font(.system(.body, design: .monospaced))
                            Spacer()
                            Text("OUI \(peer.oui)")
                                .font(.caption).foregroundColor(.secondary)
                        }
                        HStack(alignment: .firstTextBaseline) {
                            Text("Model")
                                .frame(width: 80, alignment: .leading)
                                .foregroundColor(.secondary)
                            Picker("", selection: Binding(
                                get: { driverManager.assignedModelId(peer.clockId) ?? "" },
                                set: { driverManager.assignPeerModel(peer.clockId, $0.isEmpty ? nil : $0) }
                            )) {
                                Text("Unassigned").tag("")
                                ForEach(DolbyModelCatalog.forDirection(direction)) { model in
                                    Text("\(model.displayName) · \(model.channels) ch").tag(model.id)
                                }
                            }
                            .labelsHidden()
                            .frame(maxWidth: 280, alignment: .leading)
                            Spacer()
                        }
                        Text("clock \(peer.clockId) · domain \(peer.domain) · "
                             + "\(peer.messageCount) msgs")
                            .font(.caption).foregroundColor(.secondary)
                    }
                    Divider()
                }
                let suggested = direction == .input
                    ? driverManager.suggestedInputChannelCount
                    : driverManager.suggestedOutputChannelCount
                let current = direction == .input
                    ? driverManager.rxChannelCount
                    : driverManager.txChannelCount
                let locked = driverManager.isDriverLoaded

                HStack {
                    Text("\(peers.count) found")
                        .font(.caption).foregroundColor(.secondary)
                    Spacer()
                    Text("Resolved: \(resolved) channels")
                        .font(.caption).bold()
                }
                HStack {
                    Button {
                        driverManager.applyDetectedLayout(direction)
                    } label: {
                        Text(suggested == resolved
                             ? "Set \(side) to \(suggested)"
                             : "Set \(side) to \(suggested) (rounded up from \(resolved))")
                    }
                    .disabled(locked || resolved == 0 || suggested == current)
                    .help(locked
                          ? "Uninstall the driver first — the channel layout is read when the driver starts."
                          : "Sets the device's \(side) to fit the detected elements. Takes effect on the next driver start.")
                    Spacer()
                    Text("now: \(current)")
                        .font(.caption).foregroundColor(.secondary)
                }
                Text("Vendor is read from the OUI; PTP can't tell one Dolby model from "
                     + "another, so confirm each unit's model to set its channel count. "
                     + (locked
                        ? "Uninstall the driver to apply a new layout — it's read at driver start."
                        : "Applying sets the device's \(side) to fit the detected elements, "
                          + "in groups of 8; it takes effect on the next driver start."))
                    .font(.caption).foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
    }

    // MARK: - Master tab

    @ViewBuilder
    private var masterContent: some View {
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

        section("Link") {
            parameterRow(
                "Direction",
                value: {
                    switch profile.direction {
                    case .receiveOnly:  return "Receive only"
                    case .transmitOnly: return "Transmit only"
                    case .any:          return "Send and receive"
                    }
                }(),
                locked: profile.direction != .any,
                note: {
                    switch profile.direction {
                    case .receiveOnly:
                        return "\(profile.name) sends to this driver and has no network audio "
                            + "input — everything on the Outputs tab is locked."
                    case .transmitOnly:
                        return "\(profile.name) only receives from this driver and sends nothing "
                            + "back — everything on the Inputs tab is locked."
                    case .any:
                        return "Both directions are available under this profile."
                    }
                }()
            )
            flowAddressingRow
            parameterRow(
                "DSCP marking",
                value: profile.recommendedDscp < 0
                    ? "None documented"
                    : "\(profile.recommendedDscp) (EF, factory default)",
                locked: true,
                note: profile.recommendedDscp < 0
                    ? "Nothing documented for this profile, so this driver leaves its own "
                      + "transmit traffic unmarked."
                    : "Applied — every stream this driver transmits marks its packets with "
                      + "this codepoint. Received traffic is marked by the sender, not here."
            )
        }

        section("Audio format (both directions)") {
            sampleRateRow
            parameterRow(
                "Rates this profile permits",
                value: profile.allowedSampleRates
                    .map { "\($0 / 1000) kHz" }
                    .joined(separator: ", "),
                locked: true,
                note: "Streams at any other rate are rejected under this profile."
            )
            parameterRow(
                "Packet time",
                value: profile.allowedPtimesUs.isEmpty
                    ? "any on receive · 1 ms on transmit"
                    : profile.allowedPtimesUs.map { Self.formatPtime($0) }.joined(separator: ", "),
                locked: true,
                note: profile.allowedPtimesUs.isEmpty
                    ? "This profile places no packet-time limit on received streams; "
                        + "the transmitter still emits 1 ms."
                    : "This driver's transmitter emits 1 ms packets and can't be "
                        + "reconfigured, so this is a hard limit either way."
            )
            parameterRow(
                "Encoding",
                value: profile.allowedEncodings.joined(separator: ", "),
                locked: true,
                note: "Chosen per stream from the encodings this profile accepts."
            )
        }
    }

    // MARK: - Inputs tab (RX: network -> Core Audio)

    @ViewBuilder
    private var inputsContent: some View {
        let ruledOut = profile.direction == .transmitOnly

        if ruledOut {
            directionNotice(
                "\(profile.name) is transmit-only — this driver never receives from it, so every "
                + "input parameter below is locked."
            )
        }

        section("Channels in") {
            parameterRow(
                "Input channels",
                value: ruledOut ? "Unused" : "\(driverManager.totalRxChannelCount)"
                    + (profile.maxTotalChannels > 0 && !ruledOut
                       ? " (max \(profile.maxTotalChannels))" : ""),
                locked: ruledOut,
                note: ruledOut
                    ? "The input selector on the main window is disabled under this profile."
                    : "Set with the Input selector on the main window. The device always presents "
                      + "128 channels to Core Audio; this caps how many streams may be assigned to."
            )
        }

        section("Timing") {
            VStack(alignment: .leading, spacing: 4) {
                HStack(alignment: .firstTextBaseline) {
                    Text("Playout delay")
                        .frame(width: 220, alignment: .leading)
                    if ruledOut {
                        Text("Unused")
                            .foregroundColor(.secondary)
                        Image(systemName: "lock.fill")
                            .font(.caption)
                            .foregroundColor(.secondary)
                    } else {
                        Stepper(
                            driverManager.playoutDelaySamples == 0
                                ? "Default"
                                : "\(driverManager.playoutDelaySamples) samples",
                            value: Binding(
                                get: { driverManager.playoutDelaySamples },
                                set: { driverManager.playoutDelaySamples = $0; driverManager.saveAmplifierUnit() }
                            ),
                            in: 0...DriverManager.maxPlayoutDelaySamples,
                            step: 48
                        )
                        .frame(width: 220)
                    }
                    Spacer()
                }
                Text(ruledOut
                     ? "No receive streams under this profile."
                     : "How much audio a receiver holds back before playing, as a cushion "
                       + "against network jitter. Every sample of it is latency, so raise it "
                       + "only as far as the dropouts need — 48 samples is 1 ms at 48 kHz. "
                       + "Dolby calls the same control Safety Buffer. Applies to streams "
                       + "added after the change.")
                    .font(.caption)
                    .foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }

        section("Receiving streams") {
            parameterRow(
                "Source multicast address",
                value: ruledOut
                    ? "Unused"
                    : (profile.recommendedMulticastAddress.isEmpty
                       ? "Per stream"
                       : "\(profile.recommendedMulticastAddress) (factory default)"),
                locked: ruledOut,
                note: ruledOut
                    ? "No receive streams can be added under this profile."
                    : (profile.requiredMulticastPrefix.isEmpty
                       ? "Entered per stream when adding one, or taken from an imported SDP file. "
                         + "A factory default is a starting point — installations with more than "
                         + "one auditorium on a network give each its own address."
                       : "\(profile.name) requires addresses inside "
                         + "\(profile.requiredMulticastPrefix).0.0/16 — streams outside it are "
                         + "rejected.")
            )
            parameterRow(
                "Port",
                value: ruledOut ? "Unused" : "Per stream (5004 by default)",
                locked: ruledOut,
                note: ruledOut
                    ? "No receive streams can be added under this profile."
                    : "Entered per stream when adding one; must match what the sending device uses."
            )
        }

        foundElementsSection(driverManager.inputPeers, direction: .input, side: "input channels")
    }

    // MARK: - Outputs tab (TX: Core Audio -> network)

    @ViewBuilder
    private var outputsContent: some View {
        let ruledOut = profile.direction == .receiveOnly

        if ruledOut {
            directionNotice(
                "\(profile.name) is receive-only — this driver never transmits to it, so every "
                + "output parameter below is locked."
            )
        }

        section("Channels out") {
            parameterRow(
                "Output channels",
                value: ruledOut ? "Unused" : "\(driverManager.totalTxChannelCount)"
                    + (profile.maxTotalChannels > 0 && !ruledOut
                       ? " (max \(profile.maxTotalChannels))" : ""),
                locked: ruledOut,
                note: ruledOut
                    ? "The output selector on the main window is disabled under this profile."
                    : "Set with the Output selector on the main window. Split into flows of at "
                      + "most 8 channels when transmitted."
            )
        }

        section("Transmitting streams") {
            parameterRow(
                "Destination multicast address",
                value: ruledOut
                    ? "Unused"
                    : (profile.recommendedMulticastAddress.isEmpty
                       ? "Per stream"
                       : "\(profile.recommendedMulticastAddress) (factory default)"),
                locked: ruledOut,
                note: ruledOut
                    ? "No transmit streams can be created under this profile."
                    : (profile.requiredMulticastPrefix.isEmpty
                       ? "A factory default is a starting point — installations with more than "
                         + "one auditorium on a network give each its own address."
                       : "\(profile.name) requires addresses inside "
                         + "\(profile.requiredMulticastPrefix).0.0/16.")
            )
            if !ruledOut {
                amplifierUnitRow
                sourcePortsRow
            }
        }

        foundElementsSection(driverManager.outputPeers, direction: .output, side: "output channels")
    }

    private func directionNotice(_ text: String) -> some View {
        Label(text, systemImage: "lock.fill")
            .font(.callout)
            .foregroundColor(.secondary)
            .fixedSize(horizontal: false, vertical: true)
            .padding(12)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(Color.orange.opacity(0.12))
            .cornerRadius(8)
    }

    private var sourcePortsRow: some View {
        let ports = driverManager.txSourcePorts(destinationPort: 6517)
        return parameterRow(
            "Source ports",
            value: ports.isEmpty
                ? "Assigned by the system"
                : ports.map(String.init).joined(separator: ", "),
            locked: true,
            note: ports.isEmpty
                ? "This profile tells flows apart by multicast address, so the source port "
                  + "doesn't matter and is left to the system."
                : "One per 8-channel flow, derived from the destination port (shown here for "
                  + "Dolby's own default, 6517) and the selected unit."
        )
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

    /// The device's own sample rate — selectable, or showing why it isn't.
    @ViewBuilder
    private var sampleRateRow: some View {
        let lock = driverManager.sampleRateLock
        VStack(alignment: .leading, spacing: 4) {
            HStack(alignment: .firstTextBaseline) {
                Text("Device sample rate")
                    .frame(width: 220, alignment: .leading)
                if lock == .free {
                    // Always include the device's current rate as an option,
                    // even if the profile wouldn't list it (e.g. the device
                    // came up at 192 kHz from a prior session and no profile
                    // permits it) — a SwiftUI Picker whose selection isn't
                    // among its tags shows a blank and logs a warning.
                    let options: [Double] = {
                        var r = driverManager.selectableSampleRates
                        if !r.contains(driverManager.currentDeviceSampleRate) {
                            r.append(driverManager.currentDeviceSampleRate)
                            r.sort()
                        }
                        return r
                    }()
                    Picker("", selection: Binding(
                        get: { driverManager.currentDeviceSampleRate },
                        set: { _ = driverManager.setDeviceSampleRate($0) }
                    )) {
                        ForEach(options, id: \.self) { rate in
                            let permitted = driverManager.selectableSampleRates.contains(rate)
                            Text("\(Int(rate) / 1000) kHz" + (permitted ? "" : " (not in profile)")).tag(rate)
                        }
                    }
                    .labelsHidden()
                    .frame(width: 200)
                } else {
                    Text("\(Int(driverManager.currentDeviceSampleRate) / 1000) kHz")
                        .foregroundColor(.secondary)
                    Image(systemName: "lock.fill")
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
                Spacer()
            }
            Text({
                switch lock {
                case .free:
                    return "What the device presents to Core Audio. Streams must match it — "
                         + "one that doesn't is refused rather than resampled."
                case .byProfile(let name):
                    return "\(name) permits only this rate, so there is nothing to choose."
                case .byStream(let stream):
                    return "Following the stream that's already running (\(stream)). The device "
                         + "has to match what it receives; changing it now would break that "
                         + "stream. Stop it to choose a different rate."
                }
            }())
                .font(.caption)
                .foregroundColor(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
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
                   + "unit's own source ports — see below."
                 : "\(profile.name) is a single-unit profile — nothing to select.")
                .font(.caption)
                .foregroundColor(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    /// Microseconds as the millisecond figure people actually say: 1000
    /// -> "1 ms", 125 -> "0.125 ms".
    private static func formatPtime(_ us: Int) -> String {
        let ms = Double(us) / 1000.0
        return ms == ms.rounded() ? "\(Int(ms)) ms" : "\(ms) ms"
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
