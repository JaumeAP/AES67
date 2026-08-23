import SwiftUI
import AppKit

// MARK: - PTP Health Status

enum PTPHealth: String {
    case excellent = "Excellent"  // Locked, offset < 100ns
    case good = "Good"            // Locked, offset < 1000ns
    case degraded = "Degraded"    // Locked but high offset/jitter
    case notLocked = "Not Locked" // Not synchronized
    case noMaster = "No Master"   // No PTP master found
}

struct PTPDiagnosticView: View {
    @EnvironmentObject var driverManager: DriverManager
    @State private var ptpDiagnostics: PTPDiagnostics?
    @State private var showCopiedFeedback: Bool = false
    @State private var isTestingConnectivity: Bool = false
    @State private var connectivityTestResult: ConnectivityTestResult?
    @State private var clockSources: [DriverManager.PTPClockSourceOption] = []
    @State private var isLiveData: Bool = false

    // Binding for the clock source Picker: maps the two persisted fields
    // (ptpClockSourceKind/ptpLockToDeviceUID) to a single selection ID
    // ("internal", or a device UID), and saves on every change — this is
    // the one setting on this screen that's real, not mock (see
    // DriverManager's "PTP Master Clock Source" section for why).
    private var selectedClockSourceID: Binding<String> {
        Binding(
            get: {
                driverManager.ptpClockSourceKind == "internal" ? "internal" : driverManager.ptpLockToDeviceUID
            },
            set: { newID in
                if newID == "internal" {
                    driverManager.ptpClockSourceKind = "internal"
                    driverManager.ptpLockToDeviceUID = ""
                } else {
                    driverManager.ptpClockSourceKind = "localAudioDevice"
                    driverManager.ptpLockToDeviceUID = newID
                }
                driverManager.savePTPMasterSettings()
            }
        )
    }

    // MARK: - PTP Health Computed Property

    private var ptpHealth: PTPHealth {
        guard isConnected else { return .noMaster }
        guard isLocked else { return .notLocked }
        if abs(currentOffset) < 100 { return .excellent }
        if abs(currentOffset) < 1000 { return .good }
        return .degraded
    }

    private var healthColor: Color {
        switch ptpHealth {
        case .excellent, .good:
            return .green
        case .degraded:
            return .yellow
        case .notLocked, .noMaster:
            return .red
        }
    }

    // MARK: - Health Indicator View

    @ViewBuilder
    private var healthIndicator: some View {
        HStack(spacing: 10) {
            Circle()
                .fill(healthColor)
                .frame(width: 12, height: 12)
                .shadow(color: healthColor.opacity(0.5), radius: 4)

            Text("PTP Status: \(ptpHealth.rawValue)")
                .font(.headline)

            Spacer()

            // Additional status badge
            Text(healthStatusDescription)
                .font(.caption)
                .foregroundColor(.secondary)
                .padding(.horizontal, 8)
                .padding(.vertical, 4)
                .background(healthColor.opacity(0.15))
                .cornerRadius(8)
        }
        .padding()
        .background(Color(NSColor.controlBackgroundColor))
        .cornerRadius(10)
    }

    private var healthStatusDescription: String {
        switch ptpHealth {
        case .excellent:
            return "Sub-microsecond sync"
        case .good:
            return "Synchronized"
        case .degraded:
            return "High offset detected"
        case .notLocked:
            return "Awaiting lock"
        case .noMaster:
            return "No clock source"
        }
    }

    // MARK: - Health Guidance View

    @ViewBuilder
    private var healthGuidance: some View {
        switch ptpHealth {
        case .noMaster:
            GroupBox {
                VStack(alignment: .leading, spacing: 8) {
                    Label("No PTP master clock detected on the network.", systemImage: "exclamationmark.triangle.fill")
                        .foregroundColor(.red)
                        .font(.subheadline.weight(.semibold))

                    Divider()

                    Text("Troubleshooting steps:")
                        .font(.caption.weight(.medium))
                        .foregroundColor(.secondary)

                    VStack(alignment: .leading, spacing: 6) {
                        GuidanceRow(icon: "network", text: "Check network connection and cable")
                        GuidanceRow(icon: "switch.2", text: "Ensure PTP-capable switch is configured correctly")
                        GuidanceRow(icon: "arrow.triangle.branch", text: "Verify multicast routing is enabled (IGMP snooping)")
                        GuidanceRow(icon: "flame", text: "Check firewall settings for PTP ports (319, 320)")
                        GuidanceRow(icon: "clock", text: "Confirm a PTP grandmaster is running on the network")
                    }
                }
                .padding(.vertical, 4)
            } label: {
                Label("Troubleshooting", systemImage: "wrench.and.screwdriver")
            }

        case .notLocked:
            GroupBox {
                VStack(alignment: .leading, spacing: 8) {
                    Label("PTP master found but clock not yet locked.", systemImage: "clock.badge.questionmark")
                        .foregroundColor(.orange)
                        .font(.subheadline.weight(.semibold))

                    Divider()

                    Text("What to check:")
                        .font(.caption.weight(.medium))
                        .foregroundColor(.secondary)

                    VStack(alignment: .leading, spacing: 6) {
                        GuidanceRow(icon: "timer", text: "Wait 30-60 seconds for initial synchronization")
                        GuidanceRow(icon: "waveform.path.ecg", text: "Check for network congestion or packet loss")
                        GuidanceRow(icon: "number", text: "Verify PTP domain matches your network (currently: \(currentDomain))")
                        GuidanceRow(icon: "chart.line.uptrend.xyaxis", text: "Monitor Sync/Follow-Up message counts increasing")
                    }

                    if syncMessagesReceived == 0 {
                        Text("No Sync messages received yet - check network path to master.")
                            .font(.caption)
                            .foregroundColor(.red)
                            .padding(.top, 4)
                    }
                }
                .padding(.vertical, 4)
            } label: {
                Label("Synchronization Status", systemImage: "arrow.triangle.2.circlepath")
            }

        case .degraded:
            GroupBox {
                VStack(alignment: .leading, spacing: 8) {
                    Label("Clock is locked but offset is higher than expected.", systemImage: "exclamationmark.circle")
                        .foregroundColor(.yellow)
                        .font(.subheadline.weight(.semibold))

                    Divider()

                    Text("Current offset: \(Int(currentOffset)) ns (target: < 1000 ns)")
                        .font(.caption.weight(.medium))
                        .foregroundColor(.secondary)

                    VStack(alignment: .leading, spacing: 6) {
                        GuidanceRow(icon: "network.badge.shield.half.filled", text: "Check for network asymmetry or congestion")
                        GuidanceRow(icon: "switch.2", text: "Ensure boundary clocks are properly configured")
                        GuidanceRow(icon: "cable.connector", text: "Verify cable quality and connection integrity")
                        GuidanceRow(icon: "cpu", text: "Reduce system load if CPU usage is high")
                    }

                    if offsetStdDev > 100 {
                        Text("High jitter detected (\(Int(offsetStdDev)) ns) - indicates network instability.")
                            .font(.caption)
                            .foregroundColor(.orange)
                            .padding(.top, 4)
                    }
                }
                .padding(.vertical, 4)
            } label: {
                Label("Performance Advisory", systemImage: "gauge.with.dots.needle.33percent")
            }

        case .excellent, .good:
            // Show positive feedback for good states
            GroupBox {
                VStack(alignment: .leading, spacing: 8) {
                    Label("PTP synchronization is healthy.", systemImage: "checkmark.seal.fill")
                        .foregroundColor(.green)
                        .font(.subheadline.weight(.semibold))

                    Divider()

                    HStack(spacing: 20) {
                        VStack(alignment: .leading) {
                            Text("Offset")
                                .font(.caption)
                                .foregroundColor(.secondary)
                            Text("\(Int(currentOffset)) ns")
                                .font(.system(.body, design: .monospaced))
                        }

                        VStack(alignment: .leading) {
                            Text("Jitter")
                                .font(.caption)
                                .foregroundColor(.secondary)
                            Text("\(Int(offsetStdDev)) ns")
                                .font(.system(.body, design: .monospaced))
                        }

                        VStack(alignment: .leading) {
                            Text("Freq Offset")
                                .font(.caption)
                                .foregroundColor(.secondary)
                            Text(String(format: "%.3f ppm", frequencyOffset))
                                .font(.system(.body, design: .monospaced))
                        }
                    }

                    if ptpHealth == .excellent {
                        Text("Excellent sub-microsecond synchronization achieved.")
                            .font(.caption)
                            .foregroundColor(.green)
                            .padding(.top, 4)
                    }
                }
                .padding(.vertical, 4)
            } label: {
                Label("Sync Quality", systemImage: "waveform.path.ecg")
            }
        }
    }

    // MARK: - Firewall Guidance Section

    @ViewBuilder
    private var firewallGuidanceSection: some View {
        if firewallBlockingPTP || firewallBlockingRTP {
            GroupBox {
                VStack(alignment: .leading, spacing: 12) {
                    Label("macOS firewall may be blocking network audio traffic.", systemImage: "exclamationmark.triangle.fill")
                        .foregroundColor(.orange)
                        .font(.subheadline.weight(.semibold))

                    Divider()

                    Text("Required Ports:")
                        .font(.headline)

                    VStack(alignment: .leading, spacing: 4) {
                        PortInfoRow(protocolName: "PTP Event", port: "UDP 319", blocked: firewallBlockingPTP)
                        PortInfoRow(protocolName: "PTP General", port: "UDP 320", blocked: firewallBlockingPTP)
                        PortInfoRow(protocolName: "RTP Audio", port: "UDP 5004+", blocked: firewallBlockingRTP)
                        PortInfoRow(protocolName: "SAP Discovery", port: "UDP 9875", blocked: false)
                    }
                    .padding(.leading, 4)

                    Divider()

                    Text("How to Fix:")
                        .font(.headline)

                    VStack(alignment: .leading, spacing: 6) {
                        GuidanceRow(icon: "1.circle.fill", text: "Open System Settings > Network > Firewall")
                        GuidanceRow(icon: "2.circle.fill", text: "Click 'Options...' to view firewall options")
                        GuidanceRow(icon: "3.circle.fill", text: "Add AES67 Manager to allowed apps, or use Terminal commands below")
                    }

                    Divider()

                    HStack(spacing: 12) {
                        Button(action: openFirewallSettings) {
                            Label("Open Firewall Settings", systemImage: "gear")
                        }
                        .buttonStyle(.borderedProminent)

                        Button(action: copyFirewallCommands) {
                            Label(showCopiedFeedback ? "Copied!" : "Copy Fix Commands", systemImage: showCopiedFeedback ? "checkmark" : "doc.on.doc")
                        }
                        .buttonStyle(.bordered)
                        .animation(.easeInOut(duration: 0.2), value: showCopiedFeedback)

                        Spacer()

                        Button(action: testConnectivity) {
                            if isTestingConnectivity {
                                ProgressView()
                                    .scaleEffect(0.7)
                                    .frame(width: 16, height: 16)
                                Text("Testing...")
                            } else {
                                Label("Test Connectivity", systemImage: "antenna.radiowaves.left.and.right")
                            }
                        }
                        .buttonStyle(.bordered)
                        .disabled(isTestingConnectivity)
                    }

                    // Show connectivity test result
                    if let result = connectivityTestResult {
                        connectivityTestResultView(result)
                    }
                }
                .padding(.vertical, 4)
            } label: {
                Label("Firewall Configuration Needed", systemImage: "flame.fill")
                    .foregroundColor(.orange)
            }
        }
    }

    // MARK: - Connectivity Test Result View

    @ViewBuilder
    private func connectivityTestResultView(_ result: ConnectivityTestResult) -> some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    Image(systemName: result.success ? "checkmark.circle.fill" : "xmark.circle.fill")
                        .foregroundColor(result.success ? .green : .red)
                    Text(result.success ? "Connectivity Test Passed" : "Connectivity Test Failed")
                        .font(.subheadline.weight(.semibold))
                }

                if !result.details.isEmpty {
                    ForEach(result.details, id: \.self) { detail in
                        Text("- \(detail)")
                            .font(.caption)
                            .foregroundColor(.secondary)
                    }
                }

                Text("Tested at: \(formatTime(result.timestamp))")
                    .font(.caption2)
                    .foregroundColor(.secondary)
            }
            .padding(.vertical, 4)
        }
    }

    // MARK: - Firewall Helper Functions

    private func openFirewallSettings() {
        // Try the modern System Settings URL first (macOS 13+)
        if let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Firewall") {
            NSWorkspace.shared.open(url)
        } else if let url = URL(string: "x-apple.systempreferences:com.apple.settings.PrivacySecurity.extension") {
            // Fallback for newer macOS versions
            NSWorkspace.shared.open(url)
        }
    }

    private func copyFirewallCommands() {
        let appPath = Bundle.main.bundlePath
        let commands = """
        # AES67 Manager Firewall Configuration Commands
        # Run these commands in Terminal with administrator privileges

        # Allow AES67 Manager through the macOS Application Firewall
        sudo /usr/libexec/ApplicationFirewall/socketfilterfw --add "\(appPath)"
        sudo /usr/libexec/ApplicationFirewall/socketfilterfw --unblockapp "\(appPath)"

        # Verify the app is allowed
        sudo /usr/libexec/ApplicationFirewall/socketfilterfw --listapps | grep -i aes67

        # Alternative: If using pf firewall, add these rules to /etc/pf.conf
        # pass in quick proto udp from any to any port 319  # PTP Event
        # pass in quick proto udp from any to any port 320  # PTP General
        # pass in quick proto udp from any to any port 5004:5100  # RTP Audio range
        # pass in quick proto udp from any to any port 9875 # SAP Discovery

        # To temporarily disable firewall for testing (NOT recommended for production):
        # sudo /usr/libexec/ApplicationFirewall/socketfilterfw --setglobalstate off
        # Re-enable with:
        # sudo /usr/libexec/ApplicationFirewall/socketfilterfw --setglobalstate on
        """

        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(commands, forType: .string)

        // Show feedback
        showCopiedFeedback = true
        DispatchQueue.main.asyncAfter(deadline: .now() + 2.0) {
            showCopiedFeedback = false
        }
    }

    private func testConnectivity() {
        isTestingConnectivity = true
        connectivityTestResult = nil

        // Perform connectivity test on background thread
        DispatchQueue.global(qos: .userInitiated).async {
            var details: [String] = []
            var success = true

            // Test PTP ports
            let ptpEventResult = testPort(319, protocolName: "PTP Event")
            details.append(ptpEventResult.message)
            if !ptpEventResult.success { success = false }

            let ptpGeneralResult = testPort(320, protocolName: "PTP General")
            details.append(ptpGeneralResult.message)
            if !ptpGeneralResult.success { success = false }

            // Test RTP port
            let rtpResult = testPort(5004, protocolName: "RTP Audio")
            details.append(rtpResult.message)
            if !rtpResult.success { success = false }

            // Test SAP port
            let sapResult = testPort(9875, protocolName: "SAP Discovery")
            details.append(sapResult.message)
            if !sapResult.success { success = false }

            DispatchQueue.main.async {
                self.connectivityTestResult = ConnectivityTestResult(
                    success: success,
                    details: details,
                    timestamp: Date()
                )
                self.isTestingConnectivity = false
            }
        }
    }

    private func testPort(_ port: Int, protocolName: String) -> (success: Bool, message: String) {
        // Create a UDP socket and try to bind to the port
        let socketFd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
        guard socketFd >= 0 else {
            return (false, "\(protocolName) (UDP \(port)): Failed to create socket")
        }
        defer { close(socketFd) }

        var addr = sockaddr_in()
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = in_port_t(port).bigEndian
        addr.sin_addr.s_addr = INADDR_ANY

        let bindResult = withUnsafePointer(to: &addr) { addrPtr in
            addrPtr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sockaddrPtr in
                Darwin.bind(socketFd, sockaddrPtr, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }

        if bindResult == 0 {
            return (true, "\(protocolName) (UDP \(port)): OK - Port accessible")
        } else {
            let errorCode = errno
            if errorCode == EADDRINUSE {
                return (true, "\(protocolName) (UDP \(port)): OK - Port in use (likely by this app)")
            } else if errorCode == EACCES {
                return (false, "\(protocolName) (UDP \(port)): BLOCKED - Permission denied (firewall?)")
            } else {
                return (false, "\(protocolName) (UDP \(port)): Error \(errorCode)")
            }
        }
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                HStack {
                    Text("PTP Diagnostic Information")
                        .font(.title2)
                        .fontWeight(.bold)

                    if ptpDiagnostics != nil {
                        Text(isLiveData ? "Live" : "Simulated")
                            .font(.caption2)
                            .fontWeight(.semibold)
                            .padding(.horizontal, 6)
                            .padding(.vertical, 2)
                            .background(isLiveData ? Color.green.opacity(0.2) : Color.orange.opacity(0.2))
                            .foregroundColor(isLiveData ? .green : .orange)
                            .cornerRadius(4)
                            .help(isLiveData
                                  ? "Reading the running driver's actual state"
                                  : "Driver not reachable — showing simulated data")
                    }
                }

                // Health Indicator at the top
                healthIndicator

                // Contextual Guidance
                healthGuidance

                // PTP Clock Source — real setting, persisted to
                // ptp_master.json, read by the driver at its next start.
                GroupBox("PTP Clock Source") {
                    VStack(alignment: .leading, spacing: 8) {
                        // The master switch for the whole subsystem. Until
                        // this is on, the driver runs no PTP clock at all
                        // and everything below (and every figure on this
                        // screen) is inert.
                        Toggle("Run a PTP clock", isOn: Binding(
                            get: { driverManager.ptpEnabled },
                            set: { driverManager.ptpEnabled = $0; driverManager.savePTPMasterSettings() }
                        ))
                        Text("Off by default: earlier builds compiled the PTP subsystem without "
                           + "ever starting it, so turning this on is a real change to a driver "
                           + "that has been carrying audio without it. Takes effect the next "
                           + "time Core Audio starts the driver.")
                            .font(.caption)
                            .foregroundColor(.secondary)
                            .fixedSize(horizontal: false, vertical: true)

                        Toggle("Refuse audio until the clock locks", isOn: Binding(
                            get: { driverManager.ptpRequireLock },
                            set: { driverManager.ptpRequireLock = $0; driverManager.savePTPMasterSettings() }
                        ))
                        .disabled(!driverManager.ptpEnabled)
                        .help("Streams are rejected while the PTP clock is unlocked. Stricter, "
                            + "and what the AES67 Linux daemon does unconditionally — but on a "
                            + "working system this can only ever take audio away.")

                        Divider()

                        // Locked to the active compatibility profile's PTP
                        // role (CP850 = always slave, DAC3202 = always
                        // master) — free otherwise, same fixed/free pattern
                        // as AddStreamView's PTP Domain Stepper. See
                        // DriverManager's CompatibilityProfileOption.
                        let ptpRole = driverManager.activeCompatibilityProfile.ptpRole
                        if ptpRole != .any {
                            let forcedMaster = (ptpRole == .forcedMaster)
                            HStack {
                                Image(systemName: "lock.fill")
                                    .foregroundColor(.secondary)
                                Text((forcedMaster
                                      ? "Always PTP master (fixed by "
                                      : "Always PTP slave (fixed by ")
                                     + driverManager.activeCompatibilityProfile.name + ")")
                                    .foregroundColor(.secondary)
                            }
                            .onAppear {
                                if driverManager.ptpMasterCapable != forcedMaster {
                                    driverManager.ptpMasterCapable = forcedMaster
                                    driverManager.savePTPMasterSettings()
                                }
                            }
                        } else {
                            Toggle("Act as PTP master when eligible (BMCA decides)", isOn: Binding(
                                get: { driverManager.ptpMasterCapable },
                                set: { driverManager.ptpMasterCapable = $0; driverManager.savePTPMasterSettings() }
                            ))
                        }

                        if driverManager.ptpMasterCapable {
                            Picker("Clock source:", selection: selectedClockSourceID) {
                                ForEach(clockSources) { option in
                                    Text(option.name).tag(option.id)
                                }
                                // Keep the persisted selection representable
                                // even when its device isn't present right
                                // now (Pro Tools claiming HDX is the common
                                // case) — otherwise the Picker's selection has
                                // no matching tag and renders blank.
                                if driverManager.selectedClockSourceMissing {
                                    Text("\(driverManager.ptpLockToDeviceUID) (not connected)")
                                        .tag(driverManager.ptpLockToDeviceUID)
                                }
                            }
                            .disabled(clockSources.isEmpty)

                            // The selected device is gone. Most often this
                            // is Pro Tools having claimed HDX, which is
                            // normal rather than a fault — say so, since
                            // the clock quality drops at the same moment
                            // and the cause isn't otherwise visible.
                            if driverManager.selectedClockSourceMissing {
                                Label("The selected clock device isn't available right now, so "
                                    + "this driver has fallen back to its own clock and is "
                                    + "advertising itself as a poor one. If it's Avid hardware, "
                                    + "Pro Tools has it — see below.",
                                      systemImage: "exclamationmark.triangle.fill")
                                    .font(.caption)
                                    .foregroundColor(.orange)
                                    .fixedSize(horizontal: false, vertical: true)
                            }

                            // Only shown when Avid HD hardware is actually
                            // present — there's nothing useful to say about
                            // it on a machine that doesn't have any.
                            if clockSources.contains(where: { $0.isAvidHD }) {
                                Label("Pro Tools hardware found — but Pro Tools takes it "
                                    + "exclusively and doesn't go through Core Audio, so this "
                                    + "device disappears the moment Pro Tools launches. It is "
                                    + "only usable as a clock reference while Pro Tools is "
                                    + "closed.\n\nTo actually run the network on the HDX clock, "
                                    + "the tap has to be hardware: MTRX taking DigiLink and "
                                    + "leading the audio network, or word clock out of an HD "
                                    + "interface (or Sync X) into the network's PTP "
                                    + "grandmaster. Then leave this driver on Internal and let "
                                    + "it follow that grandmaster as a slave. See "
                                    + "Docs/taking_clock_from_digilink.md.",
                                      systemImage: "exclamationmark.triangle")
                                    .font(.caption)
                                    .foregroundColor(.orange)
                                    .fixedSize(horizontal: false, vertical: true)
                            }
                        }

                        Text("Changes apply the next time Core Audio restarts, not immediately.")
                            .font(.caption)
                            .foregroundColor(.secondary)

                        HStack {
                            Button("Restart Core Audio to Apply") {
                                driverManager.restartCoreAudio()
                            }
                            .buttonStyle(.bordered)

                            Button("Refresh Device List") {
                                clockSources = driverManager.listAvailableClockSources()
                            }
                            .buttonStyle(.bordered)
                        }
                    }
                }
                .onAppear {
                    clockSources = driverManager.listAvailableClockSources()
                }

                // Grandmaster / Role — who's actually the reference clock
                // right now: this Mac itself, or a remote master we lost
                // BMCA to (or are still listening for). Still mock data
                // like the rest of this screen (see refreshDiagnostics()):
                // there's no live channel from the running driver process
                // to this app yet, only the persisted setting above.
                GroupBox("Grandmaster") {
                    VStack(alignment: .leading, spacing: 8) {
                        StatusRow(title: "Role:", status: role == .master ? "Master (this Mac is the grandmaster)" : "Slave (synced to a remote master)")
                        StatusRow(title: "Ever Was Master (this session):", status: everWasMaster ? "Yes" : "No")
                        if role == .master {
                            StatusRow(title: "Grandmaster:", status: "SELF")
                        } else {
                            StatusRow(title: "Currently Synced To:", status: masterClockID ?? "None")
                            if hasCompetitor {
                                StatusRow(title: "Lost BMCA To (priority1 / priority2):",
                                          status: "\(competitorPriority1) / \(competitorPriority2)")
                            } else {
                                StatusRow(title: "Foreign Master Heard:", status: "No")
                            }
                        }
                    }
                }

                // Connection Status
                GroupBox("Connection Status") {
                    VStack(alignment: .leading, spacing: 8) {
                        StatusRow(title: "Connected to PTP Master:", status: isConnected ? "Yes" : "No")
                        StatusRow(title: "Locked:", status: isLocked ? "Yes" : "No")
                        StatusRow(title: "Master Clock ID:", status: masterClockID ?? "Unknown")
                        StatusRow(title: "PTP Domain:", status: "\(currentDomain)")
                        StatusRow(title: "Clock Class:", status: "\(clockClass)")
                    }
                }

                // Network Diagnostics
                GroupBox("Network Diagnostics") {
                    VStack(alignment: .leading, spacing: 8) {
                        StatusRow(title: "Firewall Blocking PTP:", status: firewallBlockingPTP ? "Yes" : "No", invertColors: true)
                        StatusRow(title: "Firewall Blocking RTP:", status: firewallBlockingRTP ? "Yes" : "No", invertColors: true)
                        StatusRow(title: "Last Message Type:", status: "\(lastMessageReceived)")
                        StatusRow(title: "Last Message Time:", status: formatTime(lastMessageTime))
                    }
                }

                // Firewall Guidance (shown when firewall is blocking)
                firewallGuidanceSection

                // Quality Metrics
                GroupBox("Quality Metrics") {
                    VStack(alignment: .leading, spacing: 8) {
                        StatusRow(title: "Current Offset (ns):", status: "\(Int(currentOffset))")
                        StatusRow(title: "Mean Offset (ns):", status: "\(Int(meanOffset))")
                        StatusRow(title: "Offset Std Dev (ns):", status: "\(Int(offsetStdDev))")
                        StatusRow(title: "Frequency Offset (PPM):", status: String(format: "%.3f", frequencyOffset))
                    }
                }

                // Message Counts
                GroupBox("Message Counts") {
                    VStack(alignment: .leading, spacing: 8) {
                        StatusRow(title: "Sync Messages Received:", status: "\(syncMessagesReceived)")
                        StatusRow(title: "Follow-Up Messages Received:", status: "\(followUpMessagesReceived)")
                        StatusRow(title: "Delay Requests Sent:", status: "\(delayReqMessagesSent)")
                        StatusRow(title: "Delay Responses Received:", status: "\(delayRespMessagesReceived)")
                        StatusRow(title: "Announce Messages Received:", status: "\(announceMessagesReceived)")
                    }
                }

                // Error Counters
                GroupBox("Error Counters") {
                    VStack(alignment: .leading, spacing: 8) {
                        StatusRow(title: "State Transitions:", status: "\(stateTransitions)")
                        StatusRow(title: "Ignored Announces:", status: "\(ignoredAnnounce)")
                        StatusRow(title: "Domain Mismatch Errors:", status: "\(domainMismatchErrors)")
                    }
                }

                // Action Buttons
                HStack {
                    Button("Refresh Diagnostics") {
                        refreshDiagnostics()
                    }
                    .buttonStyle(.borderedProminent)

                    Button("Reset Counters") {
                        resetCounters()
                    }
                    .buttonStyle(.bordered)

                    Spacer()
                }
            }
            .padding()
        }
        .frame(minWidth: 550, minHeight: 700)
        .onAppear {
            refreshDiagnostics()
        }
    }
    
    private var isConnected: Bool {
        return ptpDiagnostics?.isConnected ?? false
    }
    
    private var isLocked: Bool {
        return ptpDiagnostics?.isLocked ?? false
    }
    
    private var masterClockID: String? {
        return ptpDiagnostics?.masterClockID
    }
    
    private var currentDomain: Int {
        return ptpDiagnostics?.currentDomain ?? 0
    }
    
    private var clockClass: Int {
        return ptpDiagnostics?.clockClass ?? 0
    }
    
    private var firewallBlockingPTP: Bool {
        return ptpDiagnostics?.firewallBlockingPTP ?? false
    }
    
    private var firewallBlockingRTP: Bool {
        return ptpDiagnostics?.firewallBlockingRTP ?? false
    }
    
    private var lastMessageReceived: Int {
        return ptpDiagnostics?.lastMessageReceived ?? 0
    }
    
    private var lastMessageTime: Date {
        return ptpDiagnostics?.lastMessageTime ?? Date()
    }
    
    private var currentOffset: Double {
        return ptpDiagnostics?.currentOffset ?? 0.0
    }
    
    private var meanOffset: Double {
        return ptpDiagnostics?.meanOffset ?? 0.0
    }
    
    private var offsetStdDev: Double {
        return ptpDiagnostics?.offsetStdDev ?? 0.0
    }
    
    private var frequencyOffset: Double {
        return ptpDiagnostics?.frequencyOffset ?? 0.0
    }
    
    private var syncMessagesReceived: Int {
        return ptpDiagnostics?.syncMessagesReceived ?? 0
    }
    
    private var followUpMessagesReceived: Int {
        return ptpDiagnostics?.followUpMessagesReceived ?? 0
    }
    
    private var delayReqMessagesSent: Int {
        return ptpDiagnostics?.delayReqMessagesSent ?? 0
    }
    
    private var delayRespMessagesReceived: Int {
        return ptpDiagnostics?.delayRespMessagesReceived ?? 0
    }
    
    private var announceMessagesReceived: Int {
        return ptpDiagnostics?.announceMessagesReceived ?? 0
    }
    
    private var stateTransitions: Int {
        return ptpDiagnostics?.stateTransitions ?? 0
    }
    
    private var ignoredAnnounce: Int {
        return ptpDiagnostics?.ignoredAnnounce ?? 0
    }
    
    private var domainMismatchErrors: Int {
        return ptpDiagnostics?.domainMismatchErrors ?? 0
    }

    private var role: PTPDiagnostics.Role {
        return ptpDiagnostics?.role ?? .slave
    }

    private var everWasMaster: Bool {
        return ptpDiagnostics?.everWasMaster ?? false
    }

    private var hasCompetitor: Bool {
        return ptpDiagnostics?.hasCompetitor ?? false
    }

    private var competitorPriority1: Int {
        return ptpDiagnostics?.competitorPriority1 ?? 0
    }

    private var competitorPriority2: Int {
        return ptpDiagnostics?.competitorPriority2 ?? 0
    }

    private func refreshDiagnostics() {
        // The gateway: DriverManager.fetchLivePTPDiagnostics() queries the
        // custom property AES67Device registers on itself
        // (Driver/AES67Device.cpp, Shared/CustomProperties.h) — real data
        // from the actual running driver process, not this file's old mock.
        // Only falls through to the mock below if that query fails (driver
        // not loaded, older build without the property, etc.), so the
        // screen still shows *something* rather than going blank.
        if let live = driverManager.fetchLivePTPDiagnostics() {
            ptpDiagnostics = live
            isLiveData = true
            return
        }
        isLiveData = false

        // Fallback: simulated data, same as this screen showed before the
        // gateway existed.
        ptpDiagnostics = PTPDiagnostics(
            isConnected: true,
            isLocked: true,
            masterClockID: "00-11-22-FF-FE-33-44-55",
            clockClass: 6,
            clockAccuracy: 32,
            offsetNs: 450,
            firewallBlockingPTP: false,
            firewallBlockingRTP: false,
            lastMessageReceived: 12,
            lastMessageTime: Date(),
            currentOffset: 450.0,
            meanOffset: 445.0,
            offsetStdDev: 15.0,
            frequencyOffset: 0.5,
            syncMessagesReceived: 1250,
            followUpMessagesReceived: 1248,
            delayReqMessagesSent: 890,
            delayRespMessagesReceived: 888,
            announceMessagesReceived: 120,
            stateTransitions: 2,
            ignoredAnnounce: 0,
            domainMismatchErrors: 0,
            currentDomain: 0,
            preferredDomain: 0
        )
        // Role and competitor fields aren't part of the struct's memberwise
        // init above (added after it) — set separately, still mock, but at
        // least tracking the one real setting on this screen: if master
        // capability is off, there's nothing to simulate winning BMCA with.
        if driverManager.ptpMasterCapable {
            ptpDiagnostics?.role = .master
            ptpDiagnostics?.everWasMaster = true
            ptpDiagnostics?.masterClockID = "SELF (acting as grandmaster)"
            ptpDiagnostics?.hasCompetitor = false
        } else {
            ptpDiagnostics?.role = .slave
            ptpDiagnostics?.everWasMaster = false
            ptpDiagnostics?.hasCompetitor = true
            ptpDiagnostics?.competitorPriority1 = 128
            ptpDiagnostics?.competitorPriority2 = 128
        }
    }
    
    private func resetCounters() {
        // In a real implementation, this would send a command to reset counters in the driver
        print("Resetting PTP counters...")
    }
    
    private func formatTime(_ date: Date) -> String {
        let formatter = DateFormatter()
        formatter.dateStyle = .none
        formatter.timeStyle = .medium
        return formatter.string(from: date)
    }
}

struct StatusRow: View {
    let title: String
    let status: String
    /// When true, "Yes" is bad (red) and "No" is good (green).
    /// Used for rows like "Firewall Blocking PTP" where "Yes" means a problem.
    var invertColors: Bool = false

    var body: some View {
        HStack {
            Text(title)
                .fontWeight(.medium)
            Spacer()
            Text(status)
                .foregroundColor(statusColor)
                .fontWeight(.semibold)
        }
    }

    private var statusColor: Color {
        if status == "Yes" { return invertColors ? .red : .green }
        if status == "No" { return invertColors ? .green : .red }
        if status == "Unknown" { return .orange }
        return .primary
    }
}

// MARK: - Guidance Row Helper

struct GuidanceRow: View {
    let icon: String
    let text: String

    var body: some View {
        HStack(alignment: .top, spacing: 8) {
            Image(systemName: icon)
                .font(.caption)
                .foregroundColor(.secondary)
                .frame(width: 16)

            Text(text)
                .font(.caption)
                .foregroundColor(.primary)
                .fixedSize(horizontal: false, vertical: true)
        }
    }
}

// MARK: - Port Info Row Helper

struct PortInfoRow: View {
    let protocolName: String
    let port: String
    let blocked: Bool

    var body: some View {
        HStack(spacing: 8) {
            Image(systemName: blocked ? "xmark.circle.fill" : "checkmark.circle.fill")
                .foregroundColor(blocked ? .red : .green)
                .font(.caption)

            Text("\(protocolName):")
                .font(.caption)
                .foregroundColor(.primary)

            Text(port)
                .font(.caption.monospaced())
                .foregroundColor(.secondary)

            if blocked {
                Text("BLOCKED")
                    .font(.caption2.weight(.bold))
                    .foregroundColor(.red)
                    .padding(.horizontal, 4)
                    .padding(.vertical, 2)
                    .background(Color.red.opacity(0.15))
                    .cornerRadius(4)
            }
        }
    }
}

// MARK: - Connectivity Test Result

struct ConnectivityTestResult {
    let success: Bool
    let details: [String]
    let timestamp: Date
}

#Preview("Healthy State") {
    PTPDiagnosticView()
        .environmentObject(DriverManager())
}

#Preview("No Master") {
    let view = PTPDiagnosticView()
    return view
        .environmentObject(DriverManager())
}

#Preview("Firewall Blocked") {
    // Note: In real usage, this preview would need a mock DriverManager
    // that provides ptpDiagnostics with firewallBlockingPTP = true
    PTPDiagnosticView()
        .environmentObject(DriverManager())
}