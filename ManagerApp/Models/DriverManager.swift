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
        loadAmplifierUnit()
        loadPeerAssignments()
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
        /// True for Avid HD hardware (Pro Tools HDX / HD Native).
        ///
        /// Nothing here talks DigiLink or reads the card's clock directly —
        /// that protocol is proprietary and its handshake is gated on an ID
        /// chip in genuine Avid interfaces. What makes this work is that
        /// Avid's own AudioServer publishes HDX/HD Native to CoreAudio as
        /// an ordinary device with its own hardware clock domain, so the
        /// existing "lock to a local audio device" path already reaches it.
        /// All this flag adds is recognising which device that is, so it
        /// can be offered by name and recommended, rather than sitting in
        /// the list as one more opaque entry.
        var isAvidHD: Bool = false
    }

    /// Whether Avid HD hardware is present as a clock source right now.
    ///
    /// "Right now" is load-bearing: Pro Tools takes the HDX/HD Native
    /// hardware exclusively and does not go through CoreAudio, so the
    /// device disappears from the system entirely the moment Pro Tools
    /// launches, and comes back when it quits. This can be true and false
    /// minutes apart on the same machine, without anything being wrong.
    var hasAvidHDClockSource: Bool {
        listAvailableClockSources().contains { $0.isAvidHD }
    }

    /// True when a specific device was chosen as the clock source and that
    /// device isn't currently present — Pro Tools having claimed it being
    /// the usual reason. The driver degrades honestly on its own
    /// (CoreAudioClockSource reports clockClass 248 and Unknown accuracy
    /// while the device is missing, so BMCA lets a better clock win); this
    /// is so the UI can say what happened rather than leaving the user to
    /// wonder why the clock quality dropped.
    var selectedClockSourceMissing: Bool {
        guard ptpClockSourceKind == "localAudioDevice", !ptpLockToDeviceUID.isEmpty else {
            return false
        }
        return !listAvailableClockSources().contains { $0.id == ptpLockToDeviceUID }
    }

    @Published var ptpMasterCapable: Bool = false
    /// Whether the driver runs a PTP clock at all. Off by default — the
    /// PTP subsystem was compiled and never started in every build before
    /// this, and starting it opens sockets and threads on a path verified
    /// against hardware without them.
    @Published var ptpEnabled: Bool = false
    /// Whether to refuse audio until the clock locks. Only meaningful with
    /// ptpEnabled; off by default because on a system carrying audio
    /// today it can only take audio away.
    @Published var ptpRequireLock: Bool = false
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

            // Manufacturer rather than name: a user can rename a device in
            // Audio MIDI Setup, and "HDX" appearing in some other vendor's
            // product name shouldn't be enough to claim it's Avid hardware.
            var manufacturerAddr = AudioObjectPropertyAddress(
                mSelector: kAudioObjectPropertyManufacturer,
                mScope: kAudioObjectPropertyScopeGlobal,
                mElement: kAudioObjectPropertyElementMain
            )
            var manufacturerRef: CFString? = nil
            var manufacturerSize = UInt32(MemoryLayout<CFString?>.size)
            var manufacturer = ""
            if AudioObjectGetPropertyData(device, &manufacturerAddr, 0, nil,
                                          &manufacturerSize, &manufacturerRef) == noErr,
               let m = manufacturerRef as String? {
                manufacturer = m
            }
            let isAvidHD = manufacturer.localizedCaseInsensitiveContains("avid")
                || manufacturer.localizedCaseInsensitiveContains("digidesign")

            options.append(PTPClockSourceOption(
                id: uid,
                name: isAvidHD ? "\(name) — Pro Tools hardware (gone while Pro Tools runs)" : name,
                isInternal: false,
                isAvidHD: isAvidHD
            ))
        }

        // Plain alphabetical after Internal. An earlier version promoted
        // Avid HD to the top as the recommended choice; that was wrong —
        // Pro Tools takes the hardware exclusively and the device vanishes
        // while it runs, so it is the one option that can't be relied on
        // in the room it was meant for.
        return [options[0]] + options.dropFirst().sorted {
            $0.name.localizedStandardCompare($1.name) == .orderedAscending
        }
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
        ptpEnabled = obj["ptpEnabled"] as? Bool ?? false
        ptpRequireLock = obj["requireLock"] as? Bool ?? false
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
                "ptpEnabled": ptpEnabled,
                "requireLock": ptpRequireLock,
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

    /// Selectable totals. Matches DeviceChannelSettings::allowedChannelCounts()
    /// — every group of 8 up to 128 (a usable-channel cap on the fixed
    /// 128-channel buffers), so a detected layout lands exactly instead of
    /// rounding to a coarse preset.
    static let allowedChannelCounts: [Int] = Array(stride(from: 8, through: 128, by: 8))

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
        /// Factory-default PTP domain this profile's real-world gear ships
        /// with, or -1 if none is documented. Meaningless when
        /// domainIsFixed is true. Unlike fixedDomain this is a starting
        /// point, not a lock: AddStreamView's Stepper pre-fills with this
        /// value but stays freely editable 0–127 — installers routinely
        /// pick a different domain per auditorium (e.g. 109, 110, 111,
        /// 112 for four screens on the same network) to avoid collisions.
        let recommendedPtpDomain: Int
        /// Which way this driver may talk to the described gear. AddStreamView
        /// (RX only) disables itself under .transmitOnly.
        let direction: ProfileDirection
        /// Aggregate channel ceiling in the allowed direction, 0 = unlimited.
        let maxTotalChannels: Int
        /// Which PTP role this driver must take under this profile. The
        /// "Act as PTP master" toggle is disabled and forced to match when
        /// this isn't .any.
        let ptpRole: PTPRoleConstraint
        /// How many units of this gear may be chained in one auditorium,
        /// each taking the next consecutive channel group (Dolby Atmos
        /// Connect: up to 3 without a switch). 1 = the amplifier-unit
        /// selector is meaningless and stays disabled.
        let maxUnits: Int
        /// Factory-default destination multicast address, or "" if none is
        /// documented. Informational, same as recommendedPtpDomain.
        let recommendedMulticastAddress: String
        /// Factory-default DSCP marking, or -1 if none is documented.
        let recommendedDscp: Int
        /// Sample rates this profile accepts, for display in the
        /// parameters window.
        let allowedSampleRates: [Int]
        /// Packet times this profile accepts, in MICROSECONDS — mirrors
        /// CompatibilityProfile::allowedPtimesUs. Microseconds because
        /// sub-millisecond values are real (ST 2110-30 Levels B and C are
        /// 125 us) and can't be held as integer milliseconds.
        let allowedPtimesUs: [Int]
        /// Encodings this profile accepts.
        let allowedEncodings: [String]
        /// True when this profile uses Dolby's fixed-multicast /
        /// stepped-source-port flow addressing rather than the AES67/Dante
        /// address-stepping convention.
        let usesFixedMulticastPerFlowSourcePort: Bool
        /// Multicast prefix streams must fall inside ("239.69" for Dante),
        /// or "" when any valid multicast address is accepted.
        let requiredMulticastPrefix: String
        /// True for "Dolby LAN": the driver auto-detects Dolby elements on the
        /// network (passive PTP) and shows them on the Inputs/Outputs tabs to
        /// drive the channel layout. Plain "Dolby" leaves it false — same
        /// minimal parameters, configured by hand. Default false so the other
        /// profiles' initialisers don't need to name it.
        var usesLanAutoDetection: Bool = false

        static func == (lhs: CompatibilityProfileOption, rhs: CompatibilityProfileOption) -> Bool {
            lhs.id == rhs.id
        }
    }

    /// Must stay in step with CompatibilityProfile::all() on the C++ side —
    /// there's no shared header across the language boundary.
    static let compatibilityProfiles: [CompatibilityProfileOption] =
        compatibilityProfilesGeneric + compatibilityProfilesDolby

    // Split in two so the Swift type-checker infers each smaller array
    // literal in reasonable time (one big literal times out).
    private static let compatibilityProfilesGeneric: [CompatibilityProfileOption] = [
        .init(id: "aes67",
              name: "AES67",
              caveats: "Baseline. Accepts the three sample rates AES67 names; the device "
                     + "itself declares more (up to 384 kHz), which other AES67 gear may refuse. "
                     + "PTP domain fixed at 0.",
              domainIsFixed: true, fixedDomain: 0, recommendedPtpDomain: -1,
              direction: .any, maxTotalChannels: 0, ptpRole: .any,
              maxUnits: 1, recommendedMulticastAddress: "", recommendedDscp: -1,
              allowedSampleRates: [44100, 48000, 96000], allowedPtimesUs: [1000],
              allowedEncodings: ["L16", "L24"],
              usesFixedMulticastPerFlowSourcePort: false, requiredMulticastPrefix: ""),
        .init(id: "ravenna",
              name: "RAVENNA",
              caveats: "A true AES67 superset on receive: accepts RAVENNA's full sample-rate "
                     + "set (44.1–192 kHz) and any packet time, so a RAVENNA source is not "
                     + "rejected for using a rate or ptime AES67 doesn't name. Two honest edges "
                     + "remain, both receiver-architecture limits, not RAVENNA ones: a single "
                     + "stream is still capped at 8 channels per flow (wider RAVENNA streams "
                     + "must be split), and only L16/L24 are decoded (RAVENNA's L32 is not). "
                     + "Transmit still emits 1 ms L24. RAVENNA's Bonjour discovery and stream "
                     + "redundancy are not implemented.",
              domainIsFixed: false, fixedDomain: 0, recommendedPtpDomain: -1,
              direction: .any, maxTotalChannels: 0, ptpRole: .any,
              maxUnits: 1, recommendedMulticastAddress: "", recommendedDscp: -1,
              allowedSampleRates: [44100, 48000, 88200, 96000, 176400, 192000], allowedPtimesUs: [],
              allowedEncodings: ["L16", "L24"],
              usesFixedMulticastPerFlowSourcePort: false, requiredMulticastPrefix: ""),
        .init(id: "st2110-30",
              name: "SMPTE ST 2110-30 (Level A)",
              caveats: "Level A only — Levels B and C need 125 µs packets, which this driver's "
                     + "transmitter can't emit (it is fixed at 1 ms). ST 2110-30 also requires "
                     + "stricter PTP than AES67, and this driver's PTP has never been verified "
                     + "against a real grandmaster. Enforces the parameters it can check; "
                     + "it is not a conformance claim.",
              domainIsFixed: false, fixedDomain: 0, recommendedPtpDomain: -1,
              direction: .any, maxTotalChannels: 0, ptpRole: .any,
              maxUnits: 1, recommendedMulticastAddress: "", recommendedDscp: -1,
              allowedSampleRates: [48000], allowedPtimesUs: [1000],
              allowedEncodings: ["L16", "L24"],
              usesFixedMulticastPerFlowSourcePort: false, requiredMulticastPrefix: ""),
        .init(id: "st2110-30-b",
              name: "SMPTE ST 2110-30 (Level B)",
              caveats: "Level A's constraints at a 125 µs packet time: 48 kHz, up to 8 channels "
                     + "per stream. Only choose this if the receiving gear actually claims "
                     + "Level B — a Level A device must not be sent 125 µs packets, and Level A "
                     + "is what everything supports. This driver's transmitter emits whatever "
                     + "packet time the stream asks for, so 125 µs is reachable, but it has "
                     + "never been tested against real Level B gear. Levels C (64 channels in "
                     + "one stream, more than this driver's 8-channel flow limit) and AX/BX/CX "
                     + "(96 kHz) are not offered. Same PTP caveat as Level A. Not a conformance "
                     + "claim.",
              domainIsFixed: false, fixedDomain: 0, recommendedPtpDomain: -1,
              direction: .any, maxTotalChannels: 0, ptpRole: .any,
              maxUnits: 1, recommendedMulticastAddress: "", recommendedDscp: -1,
              allowedSampleRates: [48000], allowedPtimesUs: [125],
              allowedEncodings: ["L16", "L24"],
              usesFixedMulticastPerFlowSourcePort: false, requiredMulticastPrefix: ""),
        .init(id: "dante",
              name: "Dante (AES67 mode)",
              caveats: "Requires the Dante device to have AES67 mode explicitly enabled — this "
                     + "app can't do that remotely, it's a setting on the Dante hardware itself "
                     + "(Dante Controller). Dante natively syncs with PTPv1; AES67 mode is what "
                     + "switches it to PTPv2, which is what this driver speaks, on a fixed "
                     + "domain 0. AES67 mode is narrower than AES67 itself: 48 kHz only "
                     + "(whatever the device runs natively), L24 only, 1 ms packets, port 5004. "
                     + "Enforces the 239.69.0.0/16 multicast range — that prefix is Dante's "
                     + "factory default and is configurable in Dante Controller, so a site that "
                     + "has moved it should use the AES67 baseline profile instead. Dante marks "
                     + "audio DSCP EF/46 and PTP CS7/56, where standard AES67 gear marks PTP 46 "
                     + "— a documented QoS conflict to watch for on shared networks, though "
                     + "this driver marks its own transmit traffic with Dante's audio "
                     + "value, 46.",
              domainIsFixed: true, fixedDomain: 0, recommendedPtpDomain: -1,
              direction: .any, maxTotalChannels: 0, ptpRole: .any,
              maxUnits: 1, recommendedMulticastAddress: "", recommendedDscp: 46,
              allowedSampleRates: [48000], allowedPtimesUs: [1000],
              allowedEncodings: ["L24"],
              usesFixedMulticastPerFlowSourcePort: false, requiredMulticastPrefix: "239.69"),
    ]

    private static let compatibilityProfilesDolby: [CompatibilityProfileOption] =
        [dolbyProfile, dolbyLANProfile]

    // Each on its own typed constant: with the element type explicit the
    // Swift type-checker handles the long caveats concatenation quickly, where
    // the same elements inside an array literal time out.
    private static let dolbyProfile: CompatibilityProfileOption =
        .init(id: "dolby",
              name: "Dolby",
              caveats: """
The minimal Dolby profile — the parameters common to every Dolby Atmos Connect device, for one unit you configure by hand: 48/96 kHz, 1 ms, L16/L24; PTP domain factory-default 109; destination multicast factory-default 239.81.83.67; the Atmos Connect wire scheme (one multicast address, fixed RTP destination port — pass 6517 — source port stepped per 8-channel flow); DSCP EF/46. Direction and PTP role are open — set them by how you configure the streams. For automatic discovery of Dolby gear and multi-unit chaining, use "Dolby LAN". Not a conformance claim; PTP has never been verified against real Dolby hardware.
""",
                            domainIsFixed: false, fixedDomain: 0, recommendedPtpDomain: 109,
              direction: .any, maxTotalChannels: 0, ptpRole: .any,
              maxUnits: 1, recommendedMulticastAddress: "239.81.83.67", recommendedDscp: 46,
              allowedSampleRates: [48000, 96000], allowedPtimesUs: [1000],
              allowedEncodings: ["L16", "L24"],
              usesFixedMulticastPerFlowSourcePort: true, requiredMulticastPrefix: "")

    private static let dolbyLANProfile: CompatibilityProfileOption =
        .init(id: "dolby-lan",
              name: "Dolby LAN",
              caveats: """
Dolby with automatic discovery. The driver finds Dolby elements on the network by passive PTP observation and lists them on the Inputs and Outputs tabs; confirming each detected unit's model there sets its channel count, and the resolved total becomes the device's channel layout. A master peer is a processor feeding this driver (an input, e.g. CP850/CP950); a slave peer is an amplifier this driver feeds (an output, e.g. DAC3202/DMA). The up-to-three limit is OUTPUT-side only — the amplifiers this driver feeds; input sources are not capped by it. Same family parameters as the plain Dolby profile.
""",
                            domainIsFixed: false, fixedDomain: 0, recommendedPtpDomain: 109,
              direction: .any, maxTotalChannels: 0, ptpRole: .any,
              maxUnits: 3, recommendedMulticastAddress: "239.81.83.67", recommendedDscp: 46,
              allowedSampleRates: [48000, 96000], allowedPtimesUs: [1000],
              allowedEncodings: ["L16", "L24"],
              usesFixedMulticastPerFlowSourcePort: true, requiredMulticastPrefix: "",
              usesLanAutoDetection: true)

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
              let rawId = obj["profile"] as? String else {
            compatibilityProfileID = "aes67"
            return
        }
        // Migrate the former per-model Dolby ids to the unified profile,
        // matching CompatibilityProfile::kindFromString on the driver side.
        let legacyDolby: Set<String> = ["cp850", "cp950", "dac3202", "dma"]
        let id = legacyDolby.contains(rawId) ? "dolby-lan" : rawId
        guard Self.compatibilityProfiles.contains(where: { $0.id == id }) else {
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

    // MARK: - Amplifier Unit
    //
    // Which physical unit in a chained Dolby Atmos Connect installation
    // this driver is feeding. Mirrors NetworkEngine/AmplifierUnitSettings.h
    // — same file both processes read/write (amplifier_unit.json). Only
    // meaningful when the active profile's maxUnits > 1; the driver clamps
    // whatever it reads to that, so a stale value can't affect a
    // single-unit profile.
    //
    // Each unit carries the next consecutive block of channels, which on
    // the wire means the next block of source UDP ports — so selecting
    // unit 2 shifts this driver's TX flows to that unit's ports rather
    // than changing anything about the audio itself.

    @Published var amplifierUnit: Int = 1

    /// Safety cushion in samples before a receiver starts handing audio to
    /// Core Audio. 0 = the receiver's own default. Higher survives worse
    /// network jitter and costs exactly that much latency. Stored in the
    /// same file as the amplifier unit.
    @Published var playoutDelaySamples: Int = 0
    static let maxPlayoutDelaySamples = 4800 // 100 ms at 48 kHz

    /// Units the active profile allows, as a range for the picker.
    var amplifierUnitChoices: [Int] {
        Array(1...max(1, activeCompatibilityProfile.maxUnits))
    }

    private var amplifierUnitConfigURL: URL {
        FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Application Support/AES67Driver/amplifier_unit.json")
    }

    func loadAmplifierUnit() {
        // Read the two values independently — they share this file but are
        // unrelated, and a bad/absent unit index must not throw away a valid
        // playout delay (which is what a single guard covering both did).
        // Mirrors the C++ side, where loadPlayoutDelay() reads its field
        // without regard to the unit index.
        let obj = (try? Data(contentsOf: amplifierUnitConfigURL))
            .flatMap { try? JSONSerialization.jsonObject(with: $0) as? [String: Any] } ?? [:]

        if let unit = obj["unitIndex"] as? Int, (1...3).contains(unit) {
            amplifierUnit = unit
        } else {
            amplifierUnit = 1
        }

        if let delay = obj["playoutDelaySamples"] as? Int,
           (0...Self.maxPlayoutDelaySamples).contains(delay) {
            playoutDelaySamples = delay
        } else {
            playoutDelaySamples = 0
        }
    }

    func saveAmplifierUnit() {
        let dir = amplifierUnitConfigURL.deletingLastPathComponent()
        do {
            try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
            let obj: [String: Any] = [
                "version": "1.0",
                "unitIndex": amplifierUnit,
                "playoutDelaySamples": playoutDelaySamples,
            ]
            let data = try JSONSerialization.data(withJSONObject: obj, options: [.prettyPrinted])
            try data.write(to: amplifierUnitConfigURL, options: .atomic)
        } catch {
            showAlert(title: "Save Failed",
                     message: "Could not save the amplifier unit: \(error.localizedDescription)")
        }
    }

    /// The source UDP ports this driver's TX flows will use for the
    /// currently selected unit and output channel count, or an empty array
    /// when the active profile doesn't use per-flow source ports. Mirrors
    /// StreamManager::createTxStreamFlows()'s own arithmetic so the
    /// parameters window can show what will actually go on the wire rather
    /// than a generic description of it.
    func txSourcePorts(destinationPort: Int) -> [Int] {
        let profile = activeCompatibilityProfile
        guard profile.usesFixedMulticastPerFlowSourcePort else { return [] }
        let channels = totalTxChannelCount
        let flowsPerUnit = (channels + 7) / 8
        let offset = (min(amplifierUnit, profile.maxUnits) - 1) * flowsPerUnit
        // Only ports the driver would actually use — it rolls back a flow
        // whose source port would exceed 65535 (createTxStreamFlows), so
        // showing higher ones here would misrepresent what goes on the wire.
        return (0..<flowsPerUnit)
            .map { destinationPort + 1 + offset + $0 }
            .filter { $0 <= 0xFFFF }
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

    // MARK: - SAP Discovery Gateway
    //
    // Second custom property on the same gateway — Shared/CustomProperties.h,
    // kDiscoveredSessionsPropertySelector — carrying the sessions other
    // devices are announcing over SAP. Kept in sync by hand with that
    // header, same as the diagnostics selector above.
    private static let kDiscoveredSessionsPropertySelector: AudioObjectPropertySelector = 0x61363773 // 'a67s'

    /// One AES67 session another device is announcing on the network.
    struct DiscoveredSession: Identifiable, Equatable {
        let sessionName: String
        let sourceAddress: String
        let multicastAddress: String
        let port: Int
        let ptpDomain: Int
        /// The announcer's own SDP, complete — enough to add the stream
        /// without asking the user to retype anything.
        let sdp: String

        /// Announcer + session name, which is what the driver dedupes on.
        var id: String { "\(sourceAddress)|\(sessionName)" }
    }

    @Published var discoveredSessions: [DiscoveredSession] = []

    // Third custom property on the same gateway — Shared/CustomProperties.h,
    // kPtpPeersPropertySelector ('a67e') — the passive-PTP peer list, used to
    // show which Dolby elements are on the network. Kept in sync by hand with
    // that header, same as the selectors above.
    private static let kPtpPeersPropertySelector: AudioObjectPropertySelector = 0x61363765 // 'a67e'

    /// A distinct PTP participant seen on the network. A "master" peer is a
    /// source we would follow (an input, e.g. CP850/CP950); a "slave" peer is
    /// a sink we feed (an output, e.g. DAC3202/DMA); distinct slaves are the
    /// count of chained DMA units. The vendor OUI is the first three bytes of
    /// the clock identity — how a Dolby element is recognised.
    struct DiscoveredPeer: Identifiable, Equatable {
        enum Role: String { case master, slave, mixed, unknown }
        let clockId: String
        let oui: String
        let role: Role
        let sourceIp: String
        let domain: Int
        let messageCount: Int

        var id: String { clockId }

        /// From our side: a master peer feeds us (input), a slave peer is fed
        /// by us (output). Mixed/unknown map to neither.
        var isInput: Bool { role == .master }
        var isOutput: Bool { role == .slave }
    }

    @Published var ptpPeers: [DiscoveredPeer] = []

    /// PTP peers that are sources to us (masters we would follow) — the
    /// Input side of the found-elements list.
    var inputPeers: [DiscoveredPeer] { ptpPeers.filter { $0.isInput } }

    /// PTP peers that are sinks we feed (slaves following us) — the Output
    /// side, and where the DMA unit count is read off.
    var outputPeers: [DiscoveredPeer] { ptpPeers.filter { $0.isOutput } }

    // Per-element model assignment (stage 2b). PTP gives vendor + role but not
    // model, so the user confirms each detected element's model; that maps to
    // a channel count via DolbyModelCatalog. Keyed by the peer's clock id so
    // the choice sticks to that physical unit across refreshes. Persisted in
    // UserDefaults — a ManagerApp-side preference, not a driver setting.
    @Published var peerModelAssignments: [String: String] = [:] // clockId -> modelId
    private let peerAssignmentsKey = "peerModelAssignments"

    func loadPeerAssignments() {
        if let dict = UserDefaults.standard.dictionary(forKey: peerAssignmentsKey) as? [String: String] {
            peerModelAssignments = dict
        }
    }

    /// Assign (or clear, with nil) a detected element's model and persist it.
    func assignPeerModel(_ clockId: String, _ modelId: String?) {
        if let modelId = modelId {
            peerModelAssignments[clockId] = modelId
        } else {
            peerModelAssignments.removeValue(forKey: clockId)
        }
        UserDefaults.standard.set(peerModelAssignments, forKey: peerAssignmentsKey)
    }

    /// The model id assigned to a peer, if any.
    func assignedModelId(_ clockId: String) -> String? { peerModelAssignments[clockId] }

    /// Total input channels the found+assigned input elements would contribute
    /// — what stage 2c will turn into the driver's input channel count.
    var resolvedInputChannels: Int {
        DolbyModelCatalog.totalChannels(inputPeers.compactMap { peerModelAssignments[$0.clockId] }, .input)
    }

    /// Total output channels the found+assigned output elements would contribute.
    var resolvedOutputChannels: Int {
        DolbyModelCatalog.totalChannels(outputPeers.compactMap { peerModelAssignments[$0.clockId] }, .output)
    }

    // Stage 2c: turn the resolved totals into the driver's usable channel
    // counts. The driver accepts any group of 8 up to 128 (see
    // DeviceChannelSettings::allowedChannelCounts), so a resolved total that is
    // itself a multiple of 8 — which every catalog sum is (16/24/32 units) —
    // is exposed exactly. ceilToAllowedChannelCount only rounds up when a
    // total somehow isn't group-aligned, to the next group of 8.
    static func ceilToAllowedChannelCount(_ n: Int) -> Int {
        for count in allowedChannelCounts where count >= n { return count }
        return maxDeviceChannels
    }

    /// The input channel count the detected+assigned input elements imply,
    /// rounded up to a supported size (0 stays 0 — nothing assigned).
    var suggestedInputChannelCount: Int {
        resolvedInputChannels == 0 ? 0 : Self.ceilToAllowedChannelCount(resolvedInputChannels)
    }

    /// Same for output.
    var suggestedOutputChannelCount: Int {
        resolvedOutputChannels == 0 ? 0 : Self.ceilToAllowedChannelCount(resolvedOutputChannels)
    }

    /// Apply the suggested count for one side to the device channel selection
    /// and persist it. Takes effect on the next driver start, exactly like the
    /// manual selector — the device reads device_channels.json when Core Audio
    /// constructs it. No-op if nothing is assigned on that side.
    func applyDetectedLayout(_ direction: DolbyIoDirection) {
        switch direction {
        case .input:
            let count = suggestedInputChannelCount
            guard count > 0 else { return }
            rxChannelCount = count
        case .output:
            let count = suggestedOutputChannelCount
            guard count > 0 else { return }
            txChannelCount = count
        }
        saveDeviceChannelSettings()
    }

    // MARK: - Device sample rate selection
    //
    // Three states, because there are three genuinely different reasons the
    // rate might not be the user's to pick:
    //   - free: nothing constrains it
    //   - fixed by profile: the active profile permits exactly one rate
    //   - fixed by the stream: audio is already arriving, and the device
    //     must match what it receives
    // The third is why this is phrased as "follows what it receives" rather
    // than "locked": nothing is refusing the change, the answer is simply
    // already decided by the sender.

    enum SampleRateLock: Equatable {
        case free
        case byProfile(String)   // profile name
        case byStream(String)    // stream name
    }

    /// Rates this device could be set to under the active profile — the
    /// device's own list narrowed by what the profile accepts.
    var selectableSampleRates: [Double] {
        let allowed = Set(activeCompatibilityProfile.allowedSampleRates.map(Double.init))
        let rates = Self.supportedSampleRates.filter { allowed.contains($0) }
        // A profile listing rates this device doesn't offer would otherwise
        // leave nothing selectable at all; fall back to the device's own.
        return rates.isEmpty ? Self.supportedSampleRates : rates
    }

    /// Why the sample rate can't be changed right now, if it can't.
    var sampleRateLock: SampleRateLock {
        // A running stream wins: the device has to match what's arriving,
        // and changing it underneath would break the stream that's already
        // working.
        if let stream = streams.first(where: { $0.isActive }) {
            return .byStream(stream.name)
        }
        if selectableSampleRates.count == 1 {
            return .byProfile(activeCompatibilityProfile.name)
        }
        return .free
    }

    /// Sessions currently announced, straight from the running driver.
    /// Empty when the driver isn't loaded, is an older build without this
    /// property, or simply hasn't heard any announcements yet — none of
    /// which are errors worth surfacing differently.
    func fetchDiscoveredSessions() -> [DiscoveredSession] {
        guard let deviceID = findAES67DeviceID() else { return [] }

        var address = AudioObjectPropertyAddress(
            mSelector: Self.kDiscoveredSessionsPropertySelector,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        guard AudioObjectHasProperty(deviceID, &address) else { return [] }

        var dataSize: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(deviceID, &address, 0, nil, &dataSize) == noErr else {
            return []
        }

        var cfArray: CFArray? = nil
        guard AudioObjectGetPropertyData(deviceID, &address, 0, nil, &dataSize, &cfArray) == noErr,
              let entries = cfArray as? [[String: Any]] else {
            return []
        }

        return entries.compactMap { entry in
            guard let name = entry["sessionName"] as? String,
                  let source = entry["sourceAddress"] as? String else { return nil }
            return DiscoveredSession(
                sessionName: name,
                sourceAddress: source,
                multicastAddress: entry["multicastAddress"] as? String ?? "",
                port: (entry["port"] as? Int64).map(Int.init) ?? 5004,
                ptpDomain: (entry["ptpDomain"] as? Int64).map(Int.init) ?? 0,
                sdp: entry["sdp"] as? String ?? ""
            )
        }
    }

    /// Refreshes discoveredSessions on the main thread. Called by the
    /// discovery UI while it's open rather than on the shared auto-refresh
    /// timer — there's no reason to keep querying when nobody's looking.
    func refreshDiscoveredSessions() {
        let sessions = fetchDiscoveredSessions()
        DispatchQueue.main.async {
            self.discoveredSessions = sessions
        }
    }

    /// PTP peers currently seen, straight from the running driver. Empty when
    /// the driver isn't loaded, is an older build without this property, or
    /// simply hasn't heard any PTP traffic yet — none of which are errors.
    func fetchPtpPeers() -> [DiscoveredPeer] {
        guard let deviceID = findAES67DeviceID() else { return [] }

        var address = AudioObjectPropertyAddress(
            mSelector: Self.kPtpPeersPropertySelector,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        guard AudioObjectHasProperty(deviceID, &address) else { return [] }

        var dataSize: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(deviceID, &address, 0, nil, &dataSize) == noErr else {
            return []
        }

        var cfArray: CFArray? = nil
        guard AudioObjectGetPropertyData(deviceID, &address, 0, nil, &dataSize, &cfArray) == noErr,
              let entries = cfArray as? [[String: Any]] else {
            return []
        }

        return entries.compactMap { entry in
            guard let clockId = entry["clockId"] as? String else { return nil }
            let roleStr = entry["role"] as? String ?? "unknown"
            return DiscoveredPeer(
                clockId: clockId,
                oui: entry["oui"] as? String ?? "",
                role: DiscoveredPeer.Role(rawValue: roleStr) ?? .unknown,
                sourceIp: entry["sourceIp"] as? String ?? "",
                domain: (entry["domain"] as? Int64).map(Int.init) ?? 0,
                messageCount: (entry["messageCount"] as? Int64).map(Int.init) ?? 0
            )
        }
    }

    /// Refreshes ptpPeers on the main thread. Called by the UI showing the
    /// found-elements list while it's open.
    func refreshPtpPeers() {
        let peers = fetchPtpPeers()
        DispatchQueue.main.async {
            self.ptpPeers = peers
        }
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
