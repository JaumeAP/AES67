//
// ContentView.swift
// AES67 Manager - Build #18
// Main window interface
//

import SwiftUI
import AppKit

struct ContentView: View {
    @EnvironmentObject var driverManager: DriverManager
    @EnvironmentObject var menuBarManager: MenuBarManager
    @Environment(\.openWindow) private var openWindow
    @State private var selectedStream: StreamInfo?
    @State private var showChannelMapping = false
    @State private var showChannelDiagnostic = false
    @State private var showProfileCaveats = false
    @State private var showProfileParameters = false
    @State private var showDiscoveredSessions = false

    /// One direction's channel-count selector — count picker, aux-pair
    /// toggle, running total. Used twice below (input/output), each wired to
    /// its own DriverManager state and its own disabled reason.
    @ViewBuilder
    private func channelSelector(
        label: String,
        countBinding: Binding<Int>,
        auxBinding: Binding<Bool>,
        auxFits: Bool,
        total: Int,
        disabled: Bool,
        disabledHelp: String?
    ) -> some View {
        HStack(spacing: 12) {
            Text(label)
                .font(.callout)

            Picker("", selection: countBinding) {
                ForEach(DriverManager.allowedChannelCounts, id: \.self) { count in
                    Text("\(count)").tag(count)
                }
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            .frame(width: 260)
            .disabled(disabled)

            Toggle("Aux pair (+2)", isOn: auxBinding)
                .toggleStyle(.checkbox)
                .disabled(disabled || !auxFits)
                .help(!auxFits
                      ? "No room for the auxiliary group at 128 channels — the device's buffers are fixed at 128"
                      : (disabledHelp ?? "Adds a group of 8 carrying a 2-channel auxiliary pair (6 reserved), keeping the total a multiple of 8"))

            Text("= \(total) usable")
                .font(.callout)
                .foregroundColor(.secondary)
        }
        .help(disabledHelp ?? "")
    }

    /// Input (RX, network -> Core Audio) and output (TX, Core Audio ->
    /// network) channel counts, as two independent selectors — same options
    /// and group-of-8/aux-pair semantics on each side. The device always
    /// presents all 128 channels to Core Audio in both directions; this only
    /// caps how many of them streams may actually be assigned. Only editable
    /// while the driver is uninstalled: the driver reads both once when Core
    /// Audio constructs the device.
    ///
    /// A selector is additionally disabled when the active compatibility
    /// profile rules its direction out entirely (CP850 = receive-only, so
    /// the output selector is locked; DAC3202 = transmit-only, so the input
    /// selector is locked). A hypothetical future profile fixed in both
    /// directions would simply lock both — this is a general rule, not a
    /// special case for any one profile.
    @ViewBuilder
    private var channelCountBar: some View {
        let locked = driverManager.isDriverLoaded
        let direction = driverManager.activeCompatibilityProfile.direction
        let rxRuledOut = direction == .transmitOnly
        let txRuledOut = direction == .receiveOnly
        let profileName = driverManager.activeCompatibilityProfile.name

        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 12) {
                channelSelector(
                    label: "Input:",
                    countBinding: Binding(
                        get: { driverManager.rxChannelCount },
                        set: { driverManager.rxChannelCount = $0; driverManager.saveDeviceChannelSettings() }
                    ),
                    auxBinding: Binding(
                        get: { driverManager.rxAuxChannelEnabled },
                        set: { driverManager.rxAuxChannelEnabled = $0; driverManager.saveDeviceChannelSettings() }
                    ),
                    auxFits: driverManager.rxAuxChannelFitsAtCurrentCount,
                    total: driverManager.totalRxChannelCount,
                    disabled: locked || rxRuledOut,
                    disabledHelp: rxRuledOut ? "\(profileName) is transmit-only from this driver — input is unused" : nil
                )

                Divider()
                    .frame(height: 18)

                channelSelector(
                    label: "Output:",
                    countBinding: Binding(
                        get: { driverManager.txChannelCount },
                        set: { driverManager.txChannelCount = $0; driverManager.saveDeviceChannelSettings() }
                    ),
                    auxBinding: Binding(
                        get: { driverManager.txAuxChannelEnabled },
                        set: { driverManager.txAuxChannelEnabled = $0; driverManager.saveDeviceChannelSettings() }
                    ),
                    auxFits: driverManager.txAuxChannelFitsAtCurrentCount,
                    total: driverManager.totalTxChannelCount,
                    disabled: locked || txRuledOut,
                    disabledHelp: txRuledOut ? "\(profileName) is receive-only from this driver — output is unused" : nil
                )

                Spacer()
            }

            HStack(spacing: 12) {
                // Compatibility profile: which flavour of AoIP gear this driver
                // is pointed at. Narrows what streams are accepted; never a
                // conformance claim — hence the caveats in the tooltip.
                Text("Compatible with:")
                    .font(.callout)

                // A menu rather than a flat picker so the Dolby family — the
                // generic profile, the LAN auto-detecting one, and one entry
                // per model — folds into a single "Dolby" submenu instead of
                // crowding the top level.
                Menu {
                    ForEach(DriverManager.compatibilityProfiles.filter { $0.group == nil }) { profile in
                        Button(profile.name) { selectProfile(profile.id) }
                    }
                    let dolby = DriverManager.compatibilityProfiles.filter { $0.group == "Dolby" }
                    if !dolby.isEmpty {
                        Menu("Dolby") {
                            ForEach(dolby) { profile in
                                Button(profile.name) { selectProfile(profile.id) }
                            }
                        }
                    }
                } label: {
                    HStack {
                        Text(driverManager.activeCompatibilityProfile.name)
                        Spacer()
                        Image(systemName: "chevron.up.chevron.down")
                            .font(.caption2)
                            .foregroundColor(.secondary)
                    }
                    .frame(width: 220)
                }
                .menuStyle(.borderlessButton)
                .frame(width: 220)
                .disabled(locked)
                .help(driverManager.activeCompatibilityProfile.caveats)

                Button {
                    showProfileCaveats = true
                } label: {
                    Image(systemName: "info.circle")
                }
                .buttonStyle(.borderless)
                .help("What this profile does and doesn't enforce")

                Button("Parameters…") {
                    showProfileParameters = true
                }
                .help("Every parameter this profile governs, and which of them it locks")

                // Which unit in a chained Dolby Atmos Connect installation
                // this driver is feeding — up to three chain directly, each
                // carrying the next block of channels (and, on the wire, the
                // next block of source ports). Hidden entirely rather than
                // shown disabled for the profiles where a single unit is the
                // only possibility, since there's nothing to explain there.
                if driverManager.activeCompatibilityProfile.maxUnits > 1 {
                    Divider()
                        .frame(height: 18)

                    Text("Unit:")
                        .font(.callout)

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
                    .frame(width: 120)
                    .disabled(locked)
                    .help("Which amplifier/interface in the chain this driver feeds — each unit "
                        + "takes the next block of channels and source ports")
                }

                Spacer()

                if locked {
                    Label("Turn the driver off to change", systemImage: "lock.fill")
                        .font(.caption)
                        .foregroundColor(.secondary)
                } else {
                    Text("Applies when the driver is next installed")
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
            }
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .background(.bar)
        .popover(isPresented: $showProfileCaveats) {
            VStack(alignment: .leading, spacing: 10) {
                Text(driverManager.activeCompatibilityProfile.name)
                    .font(.headline)
                Text(driverManager.activeCompatibilityProfile.caveats)
                    .font(.callout)
                    .fixedSize(horizontal: false, vertical: true)
                Text("Selecting a profile only narrows what the driver accepts. "
                   + "It is not a conformance claim.")
                    .font(.caption)
                    .foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            .padding()
            .frame(width: 380)
        }
        .sheet(isPresented: $showDiscoveredSessions) {
            DiscoveredSessionsView()
                .environmentObject(driverManager)
        }
        .sheet(isPresented: $showProfileParameters) {
            VStack(spacing: 0) {
                HStack {
                    Text("Profile Parameters")
                        .font(.title3)
                        .fontWeight(.semibold)
                    Spacer()
                    Button("Done") { showProfileParameters = false }
                }
                .padding()
                Divider()
                ProfileParametersView()
                    .environmentObject(driverManager)
            }
        }
    }

    private func selectProfile(_ id: String) {
        driverManager.compatibilityProfileID = id
        driverManager.saveCompatibilityProfile()
    }

    var body: some View {
        NavigationSplitView {
            // Sidebar - Stream List
            StreamListView(selectedStream: $selectedStream)
                .frame(minWidth: 250)
        } detail: {
            // Main content area
            if let stream = selectedStream {
                StreamDetailView(stream: stream)
            } else {
                EmptyStateView()
            }
        }
        .safeAreaInset(edge: .bottom) {
            channelCountBar
        }
        .navigationTitle("AES67 Audio Driver")
        .toolbar {
            ToolbarItemGroup {
                Button(action: { driverManager.showAddStreamSheet = true }) {
                    Label("Add Stream", systemImage: "plus")
                }

                Button(action: { driverManager.importSDPFile() }) {
                    Label("Import SDP", systemImage: "doc.badge.plus")
                }

                Button(action: { showDiscoveredSessions = true }) {
                    Label("Discover", systemImage: "antenna.radiowaves.left.and.right")
                }
                .help("Sessions other devices are announcing over SAP on this network")

                Divider()

                Button(action: { showChannelMapping = true }) {
                    Label("Channel Mapping", systemImage: "grid")
                }
                .help("Configure stream channel mappings")

                Button(action: { showChannelDiagnostic = true }) {
                    Label("Diagnostic", systemImage: "chart.bar.doc.horizontal")
                }
                .help("View 128-channel device map and diagnostics")

                Button(action: { driverManager.refreshStatus() }) {
                    Label("Refresh", systemImage: "arrow.clockwise")
                }

                Spacer()

                // Install switch: on installs the driver into the HAL, off
                // removes it. Reflects isDriverLoaded, which
                // checkDriverStatus() keeps in sync with what's actually on
                // disk — not just whatever this switch was last set to.
                Toggle(isOn: Binding(
                    get: { driverManager.isDriverLoaded },
                    set: { driverManager.setDriverInstalled($0) }
                )) {
                    Text(driverManager.isDriverLoaded ? "Driver Active" : "Driver Not Found")
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
                .toggleStyle(.switch)
                .help("Install or remove the AES67 driver from Core Audio")
            }
        }
        .sheet(isPresented: $driverManager.showAddStreamSheet) {
            AddStreamView()
                .environmentObject(driverManager)
        }
        .sheet(isPresented: $showChannelMapping) {
            ChannelMappingView(driverManager: driverManager)
                .frame(minWidth: 1200, minHeight: 800)
        }
        .sheet(isPresented: $showChannelDiagnostic) {
            ChannelMapDiagnosticView()
                .environmentObject(driverManager)
        }
        // "Open Manager" from the menu bar (MenuBarManager.openMainWindow)
        // sets showMainWindow — nothing was observing it before, so it did
        // nothing once the window had actually been closed (NSApp.activate
        // alone doesn't recreate a closed WindowGroup window). If a window
        // is already visible, just bring it forward instead of opening a
        // second one.
        .onChange(of: menuBarManager.showMainWindow) { isRequested in
            guard isRequested else { return }
            if let existing = NSApp.windows.first(where: { $0.isVisible && $0.contentViewController != nil }) {
                existing.makeKeyAndOrderFront(nil)
            } else {
                openWindow(id: "main")
            }
            NSApp.activate(ignoringOtherApps: true)
            menuBarManager.showMainWindow = false
        }
    }
}

struct EmptyStateView: View {
    @EnvironmentObject var driverManager: DriverManager

    var body: some View {
        VStack(spacing: 20) {
            Image(systemName: "waveform.circle")
                .font(.system(size: 80))
                .foregroundColor(.secondary)

            Text("No Streams Active")
                .font(.title2)
                .fontWeight(.medium)

            Text("Add an AES67 stream to get started")
                .foregroundColor(.secondary)

            HStack(spacing: 16) {
                Button("Import SDP File") {
                    driverManager.importSDPFile()
                }
                .buttonStyle(.borderedProminent)

                Button("Add Manually") {
                    driverManager.showAddStreamSheet = true
                }
            }
            .padding(.top)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Color(nsColor: .controlBackgroundColor))
    }
}

#Preview {
    let driverManager = DriverManager()
    ContentView()
        .environmentObject(driverManager)
        .environmentObject(MenuBarManager(driverManager: driverManager))
}
