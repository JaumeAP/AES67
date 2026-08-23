//
// DriverManager.swift
// AES67 Manager - Build #12
// Main interface for managing the AES67 driver
//

import Foundation
import SwiftUI
import CoreAudio

// MARK: - Pending Stream for Sample Rate Mismatch

struct PendingStreamInfo {
    let name: String
    let multicastIP: String
    let port: UInt16
    let numChannels: UInt16
    let sampleRate: UInt32
    let encoding: String
}

// MARK: - PTP Diagnostics

/// Swift representation of PTP diagnostic information
/// Mirrors the C++ PTPDiagnostics struct in NetworkEngine/PTP/PTPDiagnostics.h
struct PTPDiagnostics {
    // Connection status
    var isConnected: Bool = false
    var isLocked: Bool = false
    var masterClockID: String? = nil
    var clockClass: Int = 248
    var clockAccuracy: Int = 254
    var offsetNs: Int64 = 0

    // Network diagnostics
    var firewallBlockingPTP: Bool = false
    var firewallBlockingRTP: Bool = false
    var lastMessageReceived: Int = -1
    var lastMessageTime: Date = Date()

    // Quality metrics
    var currentOffset: Double = 0.0
    var meanOffset: Double = 0.0
    var offsetStdDev: Double = 0.0
    var frequencyOffset: Double = 0.0

    // Timing quality
    var syncMessagesReceived: Int = 0
    var followUpMessagesReceived: Int = 0
    var delayReqMessagesSent: Int = 0
    var delayRespMessagesReceived: Int = 0
    var announceMessagesReceived: Int = 0

    // Error counters
    var stateTransitions: Int = 0
    var ignoredAnnounce: Int = 0
    var domainMismatchErrors: Int = 0

    // PTP domain info
    var currentDomain: Int = 0
    var preferredDomain: Int = 0

    // Role — mirrors PTPDiagnostics::Role (NetworkEngine/PTP/PTPDiagnostics.h).
    // Only meaningful when the driver was built with master capability
    // (PTPArbitrator); a plain slave-only driver always reports .slave.
    enum Role { case slave, master }
    var role: Role = .slave
    var everWasMaster: Bool = false

    // BMCA competitor — who we're losing to (or still listening for) while
    // role == .slave. hasCompetitor is false both before anything's been
    // heard yet and once role == .master (nothing to compete with then).
    var hasCompetitor: Bool = false
    var competitorPriority1: Int = 0
    var competitorPriority2: Int = 0
}

class DriverManager: ObservableObject {
    @Published var streams: [StreamInfo] = []
    @Published var isDriverLoaded: Bool = false
    @Published var showAddStreamSheet: Bool = false
    @Published var totalChannelsUsed: Int = 0

    // Sample rate management
    @Published var currentDeviceSampleRate: Double = 48000.0
    @Published var showSampleRateMismatchAlert: Bool = false
    @Published var pendingStream: PendingStreamInfo?

    // Supported sample rates for the device
    static let supportedSampleRates: [Double] = [44100, 48000, 88200, 96000, 176400, 192000]

    private let configURL = FileManager.default.homeDirectoryForCurrentUser
        .appendingPathComponent("Library/Application Support/AES67Driver/config.json")

    private var refreshTimer: Timer?
    private var currentRefreshInterval: Double = 1.0

    init() {
        currentRefreshInterval = UserDefaults.standard.double(forKey: "refreshInterval")
        if currentRefreshInterval < 0.5 { currentRefreshInterval = 1.0 }
        checkDriverStatus()
        loadConfiguration()
        loadDeviceSampleRate()
        loadPTPMasterSettings()
        loadDeviceChannelSettings()
        loadCompatibilityProfile()
        startAutoRefresh()
    }

    // MARK: - Driver Status

    func checkDriverStatus() {
        // Check if AES67Driver.driver exists in HAL plugins
        let driverPath = "/Library/Audio/Plug-Ins/HAL/AES67Driver.driver"
        isDriverLoaded = FileManager.default.fileExists(atPath: driverPath)
    }

    func checkDriverInstallation() {
        checkDriverStatus()

        if isDriverLoaded {
            showAlert(title: "Driver Installed",
                     message: "AES67 driver is properly installed at /Library/Audio/Plug-Ins/HAL/")
        } else {
            showAlert(title: "Driver Not Found",
                     message: "Please install the AES67 driver package first.")
        }
    }

    func restartCoreAudio() {
        let task = Process()
        task.launchPath = "/bin/launchctl"
        task.arguments = ["kickstart", "-kp", "system/com.apple.audio.coreaudiod"]

        do {
            try task.run()
            task.waitUntilExit()

            if task.terminationStatus == 0 {
                DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) {
                    self.checkDriverStatus()
                }
            } else {
                // Fall back to AppleScript with privilege escalation
                restartCoreAudioWithPrivileges()
            }
        } catch {
            restartCoreAudioWithPrivileges()
        }
    }

    private func restartCoreAudioWithPrivileges() {
        let script = NSAppleScript(source: """
            do shell script "launchctl kickstart -kp system/com.apple.audio.coreaudiod" with administrator privileges
            """)
        var error: NSDictionary?
        script?.executeAndReturnError(&error)

        if let error = error {
            DispatchQueue.main.async {
                self.showAlert(title: "Restart Failed",
                             message: "Could not restart Core Audio: \(error[NSAppleScript.errorMessage] ?? "Unknown error")")
            }
        } else {
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) {
                self.checkDriverStatus()
            }
        }
    }

    // MARK: - Driver Install / Uninstall (switch-driven, not lifecycle-bound)
    //
    // This app carries its own copy of AES67Driver.driver, embedded at build
    // time into AES67Manager.app/Contents/Resources/ (see ManagerApp/build.sh).
    // Nothing here runs automatically on launch or quit: the main window has
    // a switch (ContentView) bound to isDriverLoaded that calls
    // setDriverInstalled() when the user flips it. Launch just reflects
    // whatever's actually installed (checkDriverStatus(), called from
    // init() below); quitting leaves the driver exactly as the last flip
    // left it — no forced install or removal either way.

    private static let driverName = "AES67Driver.driver"
    private static let driverInstallPath = "/Library/Audio/Plug-Ins/HAL/\(driverName)"

    /// Path to the driver bundle embedded in this app, or nil if this build
    /// of the app doesn't carry one (e.g. AES67Driver wasn't built when
    /// ManagerApp/build.sh ran).
    private var embeddedDriverURL: URL? {
        guard let url = Bundle.main.resourceURL?.appendingPathComponent(Self.driverName),
              FileManager.default.fileExists(atPath: url.path) else {
            return nil
        }
        return url
    }

    /// Bound to the main window's install switch: true installs, false
    /// uninstalls. isDriverLoaded is the source of truth for the switch's
    /// position — this only triggers the side effect, checkDriverStatus()
    /// afterwards is what actually moves the switch.
    func setDriverInstalled(_ installed: Bool) {
        if installed {
            installDriver()
        } else {
            uninstallDriver()
        }
    }

    /// Installs this app's embedded driver into the HAL and restarts Core
    /// Audio, replacing whatever was there before.
    func installDriver() {
        guard let source = embeddedDriverURL else {
            showAlert(title: "Driver Not Bundled",
                     message: "This build of AES67 Manager doesn't carry a driver to install — "
                             + "rebuild with AES67Driver.driver present before ManagerApp/build.sh runs.")
            return
        }

        // ditto, not cp -R: preserves the bundle's resource fork / extended
        // attributes, which cp -R can silently drop.
        let script = NSAppleScript(source: """
            do shell script "ditto '\(source.path)' '\(Self.driverInstallPath)' && \
            chown -R root:wheel '\(Self.driverInstallPath)' && \
            chmod -R 755 '\(Self.driverInstallPath)' && \
            launchctl kickstart -kp system/com.apple.audio.coreaudiod" with administrator privileges
            """)
        var error: NSDictionary?
        script?.executeAndReturnError(&error)

        if let error = error {
            showAlert(title: "Install Failed",
                     message: "Could not install the AES67 driver: \(error[NSAppleScript.errorMessage] ?? "Unknown error")")
        }
        // Refresh either way: the switch should reflect what's actually
        // there, not what we asked for.
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) {
            self.checkDriverStatus()
        }
    }

    /// Removes the driver from the HAL and restarts Core Audio.
    func uninstallDriver() {
        let script = NSAppleScript(source: """
            do shell script "rm -rf '\(Self.driverInstallPath)' && \
            launchctl kickstart -kp system/com.apple.audio.coreaudiod" with administrator privileges
            """)
        var error: NSDictionary?
        script?.executeAndReturnError(&error)

        if let error = error {
            showAlert(title: "Uninstall Failed",
                     message: "Could not remove the AES67 driver: \(error[NSAppleScript.errorMessage] ?? "Unknown error")")
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) {
            self.checkDriverStatus()
        }
    }

    // MARK: - PTP Master Clock Source
    //
    // This is the one piece of PTP master configuration that's real, not
    // mock: it reads/writes the same ptp_master.json file
    // PTPMasterSettingsManager reads in the driver (NetworkEngine/PTP/
    // PTPMasterSettings.h) — same directory convention as streams.json
    // (configURL above), different filename. Takes effect on the driver's
    // next start (PTPClock's constructor loads it once, at construction),
    // not live — there's no running-driver IPC channel for that yet (see
    // ptpDiagnostics below, which is still mock for exactly that reason).

    struct PTPClockSourceOption: Identifiable, Equatable {
        let id: String       // "internal", or the device's UID
        let name: String
        let isInternal: Bool
    }

    @Published var ptpMasterCapable: Bool = false
    @Published var ptpClockSourceKind: String = "internal" // "internal" | "localAudioDevice"
    @Published var ptpLockToDeviceUID: String = ""

    private var ptpMasterConfigURL: URL {
        FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Application Support/AES67Driver/ptp_master.json")
    }

    /// "Internal" plus every CoreAudio device that exposes its own clock
    /// domain (kAudioDevicePropertyClockDomain != 0) — same criterion as
    /// AudioClockDeviceList::listClockCapableAudioDevices() on the driver
    /// side, reimplemented here because this is a separate process with no
    /// way to call into the driver's C++ directly. Excludes the AES67
    /// device itself: it has no hardware clock of its own to offer back.
    func listAvailableClockSources() -> [PTPClockSourceOption] {
        var options: [PTPClockSourceOption] = [
            PTPClockSourceOption(id: "internal", name: "Internal (this Mac's clock)", isInternal: true)
        ]

        let aes67Device = findAES67DeviceID()

        var propertyAddress = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDevices,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var dataSize: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject),
                                              &propertyAddress, 0, nil, &dataSize) == noErr else {
            return options
        }
        let deviceCount = Int(dataSize) / MemoryLayout<AudioDeviceID>.size
        var devices = [AudioDeviceID](repeating: 0, count: deviceCount)
        guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject),
                                          &propertyAddress, 0, nil, &dataSize, &devices) == noErr else {
            return options
        }

        for device in devices {
            if let aes67 = aes67Device, device == aes67 { continue }

            var clockDomain: UInt32 = 0
            var clockAddr = AudioObjectPropertyAddress(
                mSelector: kAudioDevicePropertyClockDomain,
                mScope: kAudioObjectPropertyScopeGlobal,
                mElement: kAudioObjectPropertyElementMain
            )
            var size = UInt32(MemoryLayout<UInt32>.size)
            let status = AudioObjectGetPropertyData(device, &clockAddr, 0, nil, &size, &clockDomain)
            guard status == noErr, clockDomain != 0 else { continue }

            var uidAddr = AudioObjectPropertyAddress(
                mSelector: kAudioDevicePropertyDeviceUID,
                mScope: kAudioObjectPropertyScopeGlobal,
                mElement: kAudioObjectPropertyElementMain
            )
            var uidRef: CFString? = nil
            var uidSize = UInt32(MemoryLayout<CFString?>.size)
            guard AudioObjectGetPropertyData(device, &uidAddr, 0, nil, &uidSize, &uidRef) == noErr,
                  let uid = uidRef as String? else { continue }

            var nameAddr = AudioObjectPropertyAddress(
                mSelector: kAudioObjectPropertyName,
                mScope: kAudioObjectPropertyScopeGlobal,
                mElement: kAudioObjectPropertyElementMain
            )
            var nameRef: CFString? = nil
            var nameSize = UInt32(MemoryLayout<CFString?>.size)
            let name: String
            if AudioObjectGetPropertyData(device, &nameAddr, 0, nil, &nameSize, &nameRef) == noErr,
               let n = nameRef as String? {
                name = n
            } else {
                name = "(unnamed device)"
            }

            options.append(PTPClockSourceOption(id: uid, name: name, isInternal: false))
        }

        return options
    }

    /// Loads the persisted choice, defaulting to master capability off
    /// (exactly the driver's original slave-only behavior) if the file
    /// doesn't exist yet.
    func loadPTPMasterSettings() {
        guard let data = try? Data(contentsOf: ptpMasterConfigURL),
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            ptpMasterCapable = false
            ptpClockSourceKind = "internal"
            ptpLockToDeviceUID = ""
            return
        }
        ptpMasterCapable = obj["masterCapable"] as? Bool ?? false
        ptpClockSourceKind = obj["clockSourceKind"] as? String ?? "internal"
        ptpLockToDeviceUID = obj["lockToDeviceUID"] as? String ?? ""
    }

    /// Persists the current selection. The driver only reads this at
    /// startup (PTPClock's constructor) — changing it here doesn't affect
    /// an already-running driver; restart Core Audio (the existing
    /// "Restart Core Audio" action) to apply it.
    func savePTPMasterSettings() {
        let dir = ptpMasterConfigURL.deletingLastPathComponent()
        do {
            try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
            let obj: [String: Any] = [
                "version": "1.0",
                "masterCapable": ptpMasterCapable,
                "clockSourceKind": ptpClockSourceKind,
                "lockToDeviceUID": ptpLockToDeviceUID,
            ]
            let data = try JSONSerialization.data(withJSONObject: obj, options: [.prettyPrinted])
            try data.write(to: ptpMasterConfigURL, options: .atomic)
        } catch {
            showAlert(title: "Save Failed",
                     message: "Could not save the PTP clock source setting: \(error.localizedDescription)")
        }
    }

    // MARK: - Stream Management

    /// Result of attempting to add a stream
    enum AddStreamResult {
        case success
        case sampleRateMismatch(streamRate: UInt32, deviceRate: Double)
        case channelLimitExceeded
    }

    /// Adds a stream, checking for sample rate compatibility
    /// Returns the result indicating success or the reason for failure
    @discardableResult
    func addStream(name: String, multicastIP: String, port: UInt16,
                   numChannels: UInt16, sampleRate: UInt32, encoding: String,
                   ttl: UInt8 = 32, ptpDomain: Int = 0, description: String? = nil,
                   bypassSampleRateCheck: Bool = false) -> AddStreamResult {

        // Check sample rate compatibility unless bypassed
        if !bypassSampleRateCheck && sampleRate != UInt32(currentDeviceSampleRate) {
            // Store pending stream for later addition
            pendingStream = PendingStreamInfo(
                name: name,
                multicastIP: multicastIP,
                port: port,
                numChannels: numChannels,
                sampleRate: sampleRate,
                encoding: encoding
            )
            showSampleRateMismatchAlert = true
            return .sampleRateMismatch(streamRate: sampleRate, deviceRate: currentDeviceSampleRate)
        }

        // Check channel limit
        if totalChannelsUsed + Int(numChannels) > 128 {
            return .channelLimitExceeded
        }

        let stream = StreamInfo(
            id: UUID(),
            name: name,
            description: description,
            multicastIP: multicastIP,
            port: port,
            ttl: ttl,
            encoding: encoding,
            sampleRate: sampleRate,
            numChannels: numChannels,
            ptpDomain: ptpDomain
        )

        // Auto-assign device channels
        let deviceChannelStart = UInt16(totalChannelsUsed)

        let mapping = ChannelMappingInfo(
            streamID: stream.id,
            streamName: name,
            streamChannelCount: numChannels,
            deviceChannelStart: deviceChannelStart,
            deviceChannelCount: numChannels
        )

        var newStream = stream
        newStream.mapping = mapping

        streams.append(newStream)
        updateTotalChannels()
        saveConfiguration()

        return .success
    }

    /// Adds the pending stream after sample rate change confirmation
    func addPendingStream() {
        guard let pending = pendingStream else { return }

        _ = addStream(
            name: pending.name,
            multicastIP: pending.multicastIP,
            port: pending.port,
            numChannels: pending.numChannels,
            sampleRate: pending.sampleRate,
            encoding: pending.encoding,
            bypassSampleRateCheck: true
        )

        pendingStream = nil
    }

    /// Cancels the pending stream addition
    func cancelPendingStream() {
        pendingStream = nil
        showSampleRateMismatchAlert = false
    }

    func removeStream(_ stream: StreamInfo) {
        streams.removeAll { $0.id == stream.id }
        updateTotalChannels()
        saveConfiguration()
    }

    func refreshStatus() {
        checkDriverStatus()
        // In a real implementation, this would query the driver for current status
        // For now, we just refresh the driver status
    }

    // MARK: - Channel Mapping

    func assignMapping(streamID: UUID, deviceChannelStart: UInt16, deviceChannelCount: UInt16) {
        guard let index = streams.firstIndex(where: { $0.id == streamID }) else { return }

        let mapping = ChannelMappingInfo(
            streamID: streamID,
            streamName: streams[index].name,
            streamChannelCount: streams[index].numChannels,
            deviceChannelStart: deviceChannelStart,
            deviceChannelCount: deviceChannelCount
        )

        streams[index].mapping = mapping
        saveConfiguration()
    }

    func clearMapping(streamID: UUID) {
        guard let index = streams.firstIndex(where: { $0.id == streamID }) else { return }
        streams[index].mapping = nil
        saveConfiguration()
    }

    // MARK: - SDP Import/Export

    func importSDPFile() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.init(filenameExtension: "sdp")!]
        panel.allowsMultipleSelection = false
        panel.message = "Select an SDP file to import"

        panel.begin { response in
            if response == .OK, let url = panel.url {
                self.parseSDP(from: url)
            }
        }
    }

    /// Imports an SDP file directly from a URL (used for drag-and-drop)
    func importSDPFromURL(_ url: URL) {
        parseSDP(from: url)
    }

    func exportSDP(for stream: StreamInfo) {
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.init(filenameExtension: "sdp")!]
        panel.nameFieldStringValue = "\(stream.name).sdp"
        panel.message = "Export SDP file"

        panel.begin { response in
            if response == .OK, let url = panel.url {
                self.generateSDP(for: stream, to: url)
            }
        }
    }

    /// Parses an SDP file and adds the stream.
    /// Uses rtpmap-based parsing for accurate codec/channel extraction.
    private func parseSDP(from url: URL) {
        do {
            let content = try String(contentsOf: url, encoding: .utf8)
            let lines = content.components(separatedBy: .newlines)
            var name = url.deletingPathExtension().lastPathComponent
            var multicastIP = ""
            var port: UInt16 = 5004
            var numChannels: UInt16 = 2
            var sampleRate: UInt32 = 48000
            var encoding = "L24"
            var ttl: UInt8 = 32
            var ptpDomain: Int = 0

            for line in lines {
                let trimmed = line.trimmingCharacters(in: .whitespaces)
                guard !trimmed.isEmpty else { continue }

                if trimmed.hasPrefix("s=") {
                    let sessionName = String(trimmed.dropFirst(2)).trimmingCharacters(in: .whitespaces)
                    if !sessionName.isEmpty && sessionName != "-" {
                        name = sessionName
                    }
                } else if trimmed.hasPrefix("c=") {
                    let parts = trimmed.components(separatedBy: .whitespaces).filter { !$0.isEmpty }
                    if parts.count >= 3 {
                        let addrParts = parts[2].components(separatedBy: "/")
                        multicastIP = addrParts[0]
                        if addrParts.count >= 2, let parsedTTL = UInt8(addrParts[1]) {
                            ttl = parsedTTL
                        }
                    }
                } else if trimmed.hasPrefix("m=audio ") {
                    let parts = trimmed.dropFirst(8).components(separatedBy: .whitespaces).filter { !$0.isEmpty }
                    if let portStr = parts.first?.components(separatedBy: "/").first,
                       let parsedPort = UInt16(portStr), parsedPort > 0 {
                        port = parsedPort
                    }
                } else if trimmed.contains("rtpmap") {
                    if let match = trimmed.range(of: #"(L\d+|AM824)/(\d+)(?:/(\d+))?"#, options: .regularExpression) {
                        let rtpInfo = String(trimmed[match])
                        let parts = rtpInfo.components(separatedBy: "/")
                        if parts.count >= 1 { encoding = parts[0] }
                        if parts.count >= 2, let r = UInt32(parts[1]) { sampleRate = r }
                        if parts.count >= 3, let c = UInt16(parts[2]) { numChannels = c }
                    }
                } else if trimmed.contains("clock-domain:") || trimmed.contains("ptp-domain") {
                    if let match = trimmed.range(of: #"\d+"#, options: .regularExpression) {
                        if let d = Int(trimmed[match]) { ptpDomain = d }
                    }
                }
            }

            guard !multicastIP.isEmpty else {
                print("Failed to import SDP: no multicast address found")
                return
            }

            addStream(name: name, multicastIP: multicastIP, port: port,
                     numChannels: numChannels, sampleRate: sampleRate, encoding: encoding,
                     ttl: ttl, ptpDomain: ptpDomain)

        } catch {
            print("Failed to import SDP: \(error)")
        }
    }

    private func generateSDP(for stream: StreamInfo, to url: URL) {
        var sdp = "v=0\n"
        sdp += "o=- \(Int(Date().timeIntervalSince1970)) 1 IN IP4 127.0.0.1\n"
        sdp += "s=\(stream.name)\n"
        sdp += "c=IN IP4 \(stream.multicastIP)/\(stream.ttl)\n"
        sdp += "t=0 0\n"
        sdp += "m=audio \(stream.port) RTP/AVP \(stream.payloadType)\n"
        sdp += "a=rtpmap:\(stream.payloadType) \(stream.encoding)/\(stream.sampleRate)/\(stream.numChannels)\n"
        sdp += "a=ptime:1\n"

        do {
            try sdp.write(to: url, atomically: true, encoding: .utf8)
        } catch {
            print("Failed to export SDP: \(error)")
        }
    }

    // MARK: - Configuration Persistence

    private func loadConfiguration() {
        guard FileManager.default.fileExists(atPath: configURL.path) else { return }

        do {
            let data = try Data(contentsOf: configURL)
            let config = try JSONDecoder().decode(DriverConfiguration.self, from: data)

            // Convert config to streams
            streams = config.streams.compactMap { streamConfig in
                let id = UUID(uuidString: streamConfig.id) ?? UUID()
                let mapping = config.mappings.first { $0.streamID == id }

                return StreamInfo(
                    id: id,
                    name: streamConfig.name,
                    description: streamConfig.description,
                    multicastIP: streamConfig.multicastIP,
                    port: streamConfig.port,
                    encoding: streamConfig.encoding,
                    sampleRate: streamConfig.sampleRate,
                    numChannels: streamConfig.numChannels,
                    payloadType: streamConfig.payloadType,
                    ptpDomain: streamConfig.ptpDomain,
                    mapping: mapping
                )
            }

            updateTotalChannels()
        } catch {
            print("Failed to load configuration: \(error)")
        }
    }

    private func saveConfiguration() {
        // Create config directory if needed
        let configDir = configURL.deletingLastPathComponent()
        try? FileManager.default.createDirectory(at: configDir, withIntermediateDirectories: true)

        let config = DriverConfiguration(
            streams: streams.map { stream in
                StreamConfig(
                    id: stream.id.uuidString,
                    name: stream.name,
                    description: stream.description,
                    multicastIP: stream.multicastIP,
                    port: stream.port,
                    encoding: stream.encoding,
                    sampleRate: stream.sampleRate,
                    numChannels: stream.numChannels,
                    payloadType: stream.payloadType,
                    ptpDomain: stream.ptpDomain
                )
            },
            mappings: streams.compactMap { $0.mapping }
        )

        do {
            let encoder = JSONEncoder()
            encoder.outputFormatting = .prettyPrinted
            let data = try encoder.encode(config)
            try data.write(to: configURL, options: .atomic)
        } catch {
            print("Failed to save configuration: \(error)")
        }
    }

    // MARK: - Helpers

    private func updateTotalChannels() {
        totalChannelsUsed = streams.reduce(0) { $0 + Int($1.numChannels) }
    }

    private func startAutoRefresh() {
        refreshTimer?.invalidate()
        refreshTimer = Timer.scheduledTimer(withTimeInterval: currentRefreshInterval, repeats: true) { [weak self] _ in
            self?.refreshStatus()
        }
    }

    /// Updates the auto-refresh interval (called when user changes setting)
    func updateRefreshInterval(_ interval: Double) {
        guard interval >= 0.5, interval != currentRefreshInterval else { return }
        currentRefreshInterval = interval
        startAutoRefresh()
    }

    private func showAlert(title: String, message: String) {
        DispatchQueue.main.async {
            let alert = NSAlert()
            alert.messageText = title
            alert.informativeText = message
            alert.alertStyle = .informational
            alert.runModal()
        }
    }

    // MARK: - Sample Rate Management

    /// Loads the current device sample rate from CoreAudio
    private func loadDeviceSampleRate() {
        guard let deviceID = findAES67DeviceID() else {
            // Fall back to default if device not found
            currentDeviceSampleRate = 48000.0
            return
        }

        var propertyAddress = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyNominalSampleRate,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )

        var sampleRate: Float64 = 48000.0
        var dataSize = UInt32(MemoryLayout<Float64>.size)

        let status = AudioObjectGetPropertyData(
            deviceID,
            &propertyAddress,
            0,
            nil,
            &dataSize,
            &sampleRate
        )

        if status == noErr {
            currentDeviceSampleRate = sampleRate
        } else {
            print("Failed to get device sample rate: \(status)")
            currentDeviceSampleRate = 48000.0
        }
    }

    /// Sets the device sample rate
    /// - Parameter rate: The target sample rate in Hz
    /// - Returns: true if successful, false otherwise
    @discardableResult
    func setDeviceSampleRate(_ rate: Double) -> Bool {
        guard Self.supportedSampleRates.contains(rate) else {
            print("Unsupported sample rate: \(rate)")
            return false
        }

        guard let deviceID = findAES67DeviceID() else {
            print("AES67 device not found")
            return false
        }

        var propertyAddress = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyNominalSampleRate,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )

        // Check if property is settable
        var isSettable: DarwinBoolean = false
        let settableStatus = AudioObjectIsPropertySettable(deviceID, &propertyAddress, &isSettable)

        if settableStatus != noErr || !isSettable.boolValue {
            print("Sample rate property is not settable")
            return false
        }

        var sampleRate = Float64(rate)
        let dataSize = UInt32(MemoryLayout<Float64>.size)

        let status = AudioObjectSetPropertyData(
            deviceID,
            &propertyAddress,
            0,
            nil,
            dataSize,
            &sampleRate
        )

        if status == noErr {
            currentDeviceSampleRate = rate
            print("Device sample rate changed to \(Int(rate)) Hz")
            return true
        } else {
            print("Failed to set device sample rate: \(status)")
            return false
        }
    }

    /// Changes device sample rate and adds the pending stream
    func changeSampleRateAndAddPendingStream() {
        guard let pending = pendingStream else { return }

        if setDeviceSampleRate(Double(pending.sampleRate)) {
            addPendingStream()
        }

        showSampleRateMismatchAlert = false
    }

    // MARK: - Device Channel Count
    //
    // Mirrors NetworkEngine/DeviceChannelSettings.h, same file both
    // processes read/write (device_channels.json, alongside ptp_master.json).
    //
    // This does NOT change what the device advertises: Core Audio always
    // sees all 128 channels. It caps how many of them StreamChannelMapper
    // will assign to streams — declare them all, use the selected subset.
    // The driver reads it once when Core Audio constructs the device, so a
    // change only takes effect on the next start, which is why the UI
    // disables the selector while the driver is installed.

    /// Selectable totals. Matches DeviceChannelSettings::allowedChannelCounts().
    static let allowedChannelCounts: [Int] = [8, 16, 32, 64, 128]

    /// Everything is a multiple of this; the aux pair gets a whole group.
    static let channelGroupSize = 8
    /// Fixed capacity of the driver's RT ring buffers — the hard ceiling.
    static let maxDeviceChannels = 128

    // Two independent selections — input (RX, network -> Core Audio) and
    // output (TX, Core Audio -> network) — because direction isn't symmetric
    // once a compatibility profile restricts it: CP850 is receive-only from
    // this driver's own point of view, DAC3202 is transmit-only. ContentView
    // disables whichever selector the active profile rules out, alongside
    // the existing "disabled while installed" lock. Mirrors
    // NetworkEngine/DeviceChannelSettings.h's rx/tx split.
    @Published var rxChannelCount: Int = 128
    @Published var rxAuxChannelEnabled: Bool = false
    @Published var txChannelCount: Int = 128
    @Published var txAuxChannelEnabled: Bool = false

    private var deviceChannelsConfigURL: URL {
        FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Application Support/AES67Driver/device_channels.json")
    }

    /// Channels the device will actually expose on input: the RX selection
    /// plus its auxiliary group when enabled. Mirrors
    /// DeviceChannelSelection::totalChannelCount().
    var totalRxChannelCount: Int {
        let total = rxChannelCount + (rxAuxChannelEnabled ? Self.channelGroupSize : 0)
        return min(total, Self.maxDeviceChannels)
    }

    /// Same as totalRxChannelCount, for output.
    var totalTxChannelCount: Int {
        let total = txChannelCount + (txAuxChannelEnabled ? Self.channelGroupSize : 0)
        return min(total, Self.maxDeviceChannels)
    }

    /// The auxiliary group can't fit on top of 128 — the RT buffers are
    /// fixed at that size. The UI uses this to disable the checkbox rather
    /// than let the driver silently drop the request.
    var rxAuxChannelFitsAtCurrentCount: Bool {
        rxChannelCount + Self.channelGroupSize <= Self.maxDeviceChannels
    }

    /// Same as rxAuxChannelFitsAtCurrentCount, for output.
    var txAuxChannelFitsAtCurrentCount: Bool {
        txChannelCount + Self.channelGroupSize <= Self.maxDeviceChannels
    }

    func loadDeviceChannelSettings() {
        guard let data = try? Data(contentsOf: deviceChannelsConfigURL),
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            rxChannelCount = 128
            rxAuxChannelEnabled = false
            txChannelCount = 128
            txAuxChannelEnabled = false
            return
        }
        let rxCount = obj["rxChannelCount"] as? Int ?? 128
        rxChannelCount = Self.allowedChannelCounts.contains(rxCount) ? rxCount : 128
        rxAuxChannelEnabled = (obj["rxAuxChannelEnabled"] as? Bool ?? false) && rxAuxChannelFitsAtCurrentCount

        let txCount = obj["txChannelCount"] as? Int ?? 128
        txChannelCount = Self.allowedChannelCounts.contains(txCount) ? txCount : 128
        txAuxChannelEnabled = (obj["txAuxChannelEnabled"] as? Bool ?? false) && txAuxChannelFitsAtCurrentCount
    }

    func saveDeviceChannelSettings() {
        // Keep the fields consistent before writing: the driver's
        // DeviceChannelSelection::isValid() rejects aux-at-128 outright, and
        // a rejected file silently falls back to defaults — worse than
        // correcting it here.
        if !rxAuxChannelFitsAtCurrentCount { rxAuxChannelEnabled = false }
        if !txAuxChannelFitsAtCurrentCount { txAuxChannelEnabled = false }

        let dir = deviceChannelsConfigURL.deletingLastPathComponent()
        do {
            try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
            let obj: [String: Any] = [
                "version": "1.0",
                "rxChannelCount": rxChannelCount,
                "rxAuxChannelEnabled": rxAuxChannelEnabled,
                "txChannelCount": txChannelCount,
                "txAuxChannelEnabled": txAuxChannelEnabled,
            ]
            let data = try JSONSerialization.data(withJSONObject: obj, options: [.prettyPrinted])
            try data.write(to: deviceChannelsConfigURL, options: .atomic)
        } catch {
            showAlert(title: "Save Failed",
                     message: "Could not save the channel count setting: \(error.localizedDescription)")
        }
    }

    // MARK: - Compatibility Profile
    //
    // Which flavour of AoIP gear the driver is being pointed at. Mirrors
    // NetworkEngine/CompatibilityProfile.h — same file both processes
    // read/write (compatibility_profile.json). The driver reads it when
    // Core Audio constructs the device and applies it to every stream added
    // from then on, so a change takes effect on the next start.
    //
    // Selecting a profile only ever NARROWS what the driver accepts. It is
    // not a conformance claim: each profile carries its own caveats, shown
    // in the UI, about what it can't enforce.

    /// Mirrors CompatibilityProfile's ProfileDirection (CompatibilityProfile.h)
    /// — always from THIS driver's own point of view. .receiveOnly means we
    /// may receive from the remote device, never transmit to it.
    enum ProfileDirection {
        case any, receiveOnly, transmitOnly
    }

    /// Mirrors CompatibilityProfile's PTPRoleConstraint (CompatibilityProfile.h)
    /// — always from THIS driver's own point of view. Not enforced by the
    /// driver today (the PTP arbitrator isn't wired into the real driver
    /// path yet); this only locks the "Act as PTP master" toggle
    /// (PTPDiagnosticView) to the right value while the profile is active.
    enum PTPRoleConstraint {
        case any, forcedSlave, forcedMaster
    }

    struct CompatibilityProfileOption: Identifiable, Equatable {
        let id: String       // matches CompatibilityProfile::kindToString()
        let name: String
        let caveats: String
        /// True when the C++ profile pins PTP domain to fixedDomain — the
        /// AddStreamView Stepper is disabled and forced to that value in
        /// that case, editable 0–127 otherwise.
        let domainIsFixed: Bool
        let fixedDomain: Int
        /// Which way this driver may talk to the described gear. AddStreamView
        /// (RX only) disables itself under .transmitOnly.
        let direction: ProfileDirection
        /// Aggregate channel ceiling in the allowed direction, 0 = unlimited.
        let maxTotalChannels: Int
        /// Which PTP role this driver must take under this profile. The
        /// "Act as PTP master" toggle is disabled and forced to match when
        /// this isn't .any.
        let ptpRole: PTPRoleConstraint

        static func == (lhs: CompatibilityProfileOption, rhs: CompatibilityProfileOption) -> Bool {
            lhs.id == rhs.id
        }
    }

    /// Must stay in step with CompatibilityProfile::all() on the C++ side —
    /// there's no shared header across the language boundary.
    static let compatibilityProfiles: [CompatibilityProfileOption] = [
        .init(id: "aes67",
              name: "AES67",
              caveats: "Baseline. Accepts the three sample rates AES67 names; the device "
                     + "itself declares more (up to 384 kHz), which other AES67 gear may refuse. "
                     + "PTP domain fixed at 0.",
              domainIsFixed: true, fixedDomain: 0,
              direction: .any, maxTotalChannels: 0, ptpRole: .any),
        .init(id: "ravenna",
              name: "RAVENNA",
              caveats: "Constraints are currently identical to AES67: RAVENNA is more permissive, "
                     + "not less, and the extra freedom (1–192 samples per packet) needs a "
                     + "configurable transmit packet time this driver doesn't have yet. "
                     + "RAVENNA's own additions — Bonjour discovery and stream redundancy — "
                     + "are not implemented.",
              domainIsFixed: false, fixedDomain: 0,
              direction: .any, maxTotalChannels: 0, ptpRole: .any),
        .init(id: "st2110-30",
              name: "SMPTE ST 2110-30 (Level A)",
              caveats: "Level A only — Levels B and C need 125 µs packets, which this driver's "
                     + "transmitter can't emit (it is fixed at 1 ms). ST 2110-30 also requires "
                     + "stricter PTP than AES67, and this driver's PTP has never been verified "
                     + "against a real grandmaster. Enforces the parameters it can check; "
                     + "it is not a conformance claim.",
              domainIsFixed: false, fixedDomain: 0,
              direction: .any, maxTotalChannels: 0, ptpRole: .any),
        .init(id: "dante",
              name: "Dante (AES67 mode)",
              caveats: "Requires the Dante device to have AES67 mode explicitly enabled — this "
                     + "app can't do that remotely, it's a setting on the Dante hardware itself "
                     + "(Dante Controller). Dante natively syncs with PTPv1; AES67 mode is what "
                     + "switches it to PTPv2, which is what this driver speaks. Enforces the "
                     + "239.69.0.0/16 multicast range Dante requires in AES67 mode.",
              domainIsFixed: false, fixedDomain: 0,
              direction: .any, maxTotalChannels: 0, ptpRole: .any),
        .init(id: "cp850",
              name: "Dolby CP850 (Atmos Cinema Processor)",
              caveats: "Uses AES67 as its transport to Dolby Atmos Connect Interfaces (DAC3202), "
                     + "not the full Dante protocol. Dolby's own documentation notes it applies "
                     + "a more traditional DSCP marking than typical Dante configs (EF/46) — "
                     + "this driver has a DSCP-setting function but nothing calls it yet, so no "
                     + "marking is actually applied. No documented fixed PTP domain. This driver "
                     + "is always PTP slave under this profile — it never contends for "
                     + "grandmaster. Receive-only: this driver may only add RX streams under "
                     + "this profile, up to 64 channels total, the most the CP850 renders.",
              domainIsFixed: false, fixedDomain: 0,
              direction: .receiveOnly, maxTotalChannels: 64, ptpRole: .forcedSlave),
        .init(id: "dac3202",
              name: "Dolby DAC3202 (Atmos Connect Interface)",
              caveats: "Receiving end of the same CP850 link — 32 analog outputs, so a full-width "
                     + "feed is 4 flows of 8 channels under this driver's flow splitter. Same "
                     + "DSCP note as CP850: documented as EF/46 but not actually applied. This "
                     + "driver is always PTP master under this profile. Transmit-only: this "
                     + "driver may only create TX streams under this profile, up to 32 channels "
                     + "total.",
              domainIsFixed: false, fixedDomain: 0,
              direction: .transmitOnly, maxTotalChannels: 32, ptpRole: .forcedMaster),
    ]

    @Published var compatibilityProfileID: String = "aes67"

    var activeCompatibilityProfile: CompatibilityProfileOption {
        Self.compatibilityProfiles.first { $0.id == compatibilityProfileID }
            ?? Self.compatibilityProfiles[0]
    }

    private var compatibilityProfileConfigURL: URL {
        FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Application Support/AES67Driver/compatibility_profile.json")
    }

    func loadCompatibilityProfile() {
        guard let data = try? Data(contentsOf: compatibilityProfileConfigURL),
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let id = obj["profile"] as? String,
              Self.compatibilityProfiles.contains(where: { $0.id == id }) else {
            // Unknown or missing falls back to the unrestricted baseline —
            // never to a profile that would start rejecting working streams.
            compatibilityProfileID = "aes67"
            return
        }
        compatibilityProfileID = id
    }

    func saveCompatibilityProfile() {
        let dir = compatibilityProfileConfigURL.deletingLastPathComponent()
        do {
            try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
            let obj: [String: Any] = ["version": "1.0", "profile": compatibilityProfileID]
            let data = try JSONSerialization.data(withJSONObject: obj, options: [.prettyPrinted])
            try data.write(to: compatibilityProfileConfigURL, options: .atomic)
        } catch {
            showAlert(title: "Save Failed",
                     message: "Could not save the compatibility profile: \(error.localizedDescription)")
        }
    }

    // MARK: - PTP Diagnostics Gateway
    //
    // Reads AES67Device's custom property — Shared/CustomProperties.h,
    // kPTPDiagnosticsPropertySelector — the live bridge from the running
    // driver process (inside coreaudiod) to this app. AudioObjectGetPropertyData
    // is already cross-process, so this needed no XPC or shared files: the
    // driver registers the property (RegisterCustomProperty in
    // Driver/AES67Device.cpp), any client queries it.
    //
    // Must match Shared/CustomProperties.h's kPTPDiagnosticsPropertySelector
    // exactly — there's no shared header across the C++/Swift boundary, so
    // this is kept in sync by hand.
    private static let kPTPDiagnosticsPropertySelector: AudioObjectPropertySelector = 0x61363764 // 'a67d'

    /// Live diagnostics from the running driver, or nil if the driver isn't
    /// loaded, doesn't expose this property (e.g. an older build without
    /// this gateway), or the query otherwise fails. Callers should fall
    /// back to something else — see PTPDiagnosticView.refreshDiagnostics().
    func fetchLivePTPDiagnostics() -> PTPDiagnostics? {
        guard let deviceID = findAES67DeviceID() else { return nil }

        var address = AudioObjectPropertyAddress(
            mSelector: Self.kPTPDiagnosticsPropertySelector,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        guard AudioObjectHasProperty(deviceID, &address) else { return nil }

        var dataSize: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(deviceID, &address, 0, nil, &dataSize) == noErr else {
            return nil
        }

        var cfDict: CFDictionary? = nil
        guard AudioObjectGetPropertyData(deviceID, &address, 0, nil, &dataSize, &cfDict) == noErr,
              let dict = cfDict as? [String: Any] else {
            return nil
        }

        var diag = PTPDiagnostics()
        diag.isConnected = dict["isConnected"] as? Bool ?? false
        diag.isLocked = dict["isLocked"] as? Bool ?? false
        diag.masterClockID = dict["masterClockID"] as? String
        diag.clockClass = (dict["clockClass"] as? Int64).map(Int.init) ?? 248
        diag.clockAccuracy = (dict["clockAccuracy"] as? Int64).map(Int.init) ?? 254
        diag.offsetNs = dict["offsetNs"] as? Int64 ?? 0
        diag.currentDomain = (dict["currentDomain"] as? Int64).map(Int.init) ?? 0
        diag.role = (dict["role"] as? String) == "master" ? .master : .slave
        diag.everWasMaster = dict["everWasMaster"] as? Bool ?? false
        diag.hasCompetitor = dict["hasCompetitor"] as? Bool ?? false
        diag.competitorPriority1 = (dict["competitorPriority1"] as? Int64).map(Int.init) ?? 0
        diag.competitorPriority2 = (dict["competitorPriority2"] as? Int64).map(Int.init) ?? 0
        diag.syncMessagesReceived = (dict["syncMessagesReceived"] as? Int64).map(Int.init) ?? 0
        diag.announceMessagesReceived = (dict["announceMessagesReceived"] as? Int64).map(Int.init) ?? 0
        diag.currentOffset = Double(diag.offsetNs)
        return diag
    }

    /// Finds the AES67 virtual audio device ID
    private func findAES67DeviceID() -> AudioDeviceID? {
        var propertyAddress = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDevices,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )

        var dataSize: UInt32 = 0
        var status = AudioObjectGetPropertyDataSize(
            AudioObjectID(kAudioObjectSystemObject),
            &propertyAddress,
            0,
            nil,
            &dataSize
        )

        guard status == noErr else { return nil }

        let deviceCount = Int(dataSize) / MemoryLayout<AudioDeviceID>.size
        var devices = [AudioDeviceID](repeating: 0, count: deviceCount)

        status = AudioObjectGetPropertyData(
            AudioObjectID(kAudioObjectSystemObject),
            &propertyAddress,
            0,
            nil,
            &dataSize,
            &devices
        )

        guard status == noErr else { return nil }

        // Find AES67 device by name
        for device in devices {
            var namePropertyAddress = AudioObjectPropertyAddress(
                mSelector: kAudioDevicePropertyDeviceName,
                mScope: kAudioObjectPropertyScopeGlobal,
                mElement: kAudioObjectPropertyElementMain
            )

            var nameSize: UInt32 = 0
            status = AudioObjectGetPropertyDataSize(device, &namePropertyAddress, 0, nil, &nameSize)
            guard status == noErr else { continue }

            var name = [CChar](repeating: 0, count: Int(nameSize))
            status = AudioObjectGetPropertyData(device, &namePropertyAddress, 0, nil, &nameSize, &name)
            guard status == noErr else { continue }

            let deviceName = String(cString: name)
            if deviceName.contains("AES67") {
                return device
            }
        }

        return nil
    }

    /// Returns a formatted string for the sample rate
    static func formatSampleRate(_ rate: Double) -> String {
        if rate >= 1000 {
            let khz = rate / 1000.0
            if khz.truncatingRemainder(dividingBy: 1) == 0 {
                return "\(Int(khz)) kHz"
            } else {
                return String(format: "%.1f kHz", khz)
            }
        }
        return "\(Int(rate)) Hz"
    }

    deinit {
        refreshTimer?.invalidate()
    }
}
