//
// MenuBarManager.swift
// AES67 Manager - Menu Bar Integration
//
// Provides menu bar icon and quick access to driver controls
//

import SwiftUI
import AppKit
import ServiceManagement

class MenuBarManager: NSObject, ObservableObject {
    private var statusItem: NSStatusItem?
    private var driverManager: DriverManager
    @Published var showMainWindow = false

    init(driverManager: DriverManager) {
        self.driverManager = driverManager
        super.init()
        setupMenuBar()
    }

    func setupMenuBar() {
        // Create status bar item
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)

        if let button = statusItem?.button {
            // Use system audio icon
            button.image = NSImage(systemSymbolName: "waveform.circle.fill", accessibilityDescription: "AES67 Audio")
            button.image?.isTemplate = true
            button.toolTip = "AES67 Audio Driver"
        }

        // Create menu
        updateMenu()

        // Update menu periodically
        Timer.scheduledTimer(withTimeInterval: 2.0, repeats: true) { [weak self] _ in
            self?.updateMenu()
        }
    }

    func updateMenu() {
        let menu = NSMenu(title: "AES67")

        // Status section
        let streamCount = driverManager.streams.count
        let statusTitle = streamCount == 0 ? "No Streams" : "\(streamCount) Stream\(streamCount == 1 ? "" : "s")"
        let statusMenuItem = NSMenuItem(title: statusTitle, action: nil, keyEquivalent: "")
        statusMenuItem.isEnabled = false
        menu.addItem(statusMenuItem)

        // Driver status
        let driverStatus = driverManager.isDriverLoaded ? "✓ Driver Loaded" : "✗ Driver Not Loaded"
        let driverItem = NSMenuItem(title: driverStatus, action: nil, keyEquivalent: "")
        driverItem.isEnabled = false
        menu.addItem(driverItem)

        menu.addItem(NSMenuItem.separator())

        // Quick actions
        menu.addItem(NSMenuItem(title: "Open Manager", action: #selector(openMainWindow), keyEquivalent: "o"))
        menu.addItem(NSMenuItem(title: "Add Stream...", action: #selector(addStream), keyEquivalent: "n"))

        menu.addItem(NSMenuItem.separator())

        // Stream list (max 5)
        if streamCount > 0 {
            let streamsToShow = min(5, streamCount)
            for i in 0..<streamsToShow {
                let stream = driverManager.streams[i]
                let streamItem = NSMenuItem(
                    title: "  \(stream.name) (\(stream.multicastIP):\(stream.port))",
                    action: nil,
                    keyEquivalent: ""
                )
                streamItem.isEnabled = false
                menu.addItem(streamItem)
            }

            if streamCount > 5 {
                let moreItem = NSMenuItem(title: "  ... and \(streamCount - 5) more", action: nil, keyEquivalent: "")
                moreItem.isEnabled = false
                menu.addItem(moreItem)
            }

            menu.addItem(NSMenuItem.separator())
        }

        // Settings
        menu.addItem(NSMenuItem(title: "Preferences...", action: #selector(openPreferences), keyEquivalent: ","))

        menu.addItem(NSMenuItem.separator())

        // Launch at login
        let launchAtLoginItem = NSMenuItem(
            title: "Launch at Login",
            action: #selector(toggleLaunchAtLogin),
            keyEquivalent: ""
        )
        launchAtLoginItem.state = isLaunchAtLoginEnabled() ? .on : .off
        menu.addItem(launchAtLoginItem)

        menu.addItem(NSMenuItem.separator())

        // Quit
        menu.addItem(NSMenuItem(title: "Quit AES67 Manager", action: #selector(quit), keyEquivalent: "q"))

        // Set target for menu items
        for item in menu.items {
            item.target = self
        }

        // Assign menu to status item
        if let item = statusItem {
            item.menu = menu
        }
    }

    @objc func openMainWindow() {
        showMainWindow = true
        NSApp.activate(ignoringOtherApps: true)
    }

    @objc func addStream() {
        showMainWindow = true
        NSApp.activate(ignoringOtherApps: true)
        // Post notification to show add stream sheet
        NotificationCenter.default.post(name: NSNotification.Name("ShowAddStream"), object: nil)
    }

    @objc func openPreferences() {
        showMainWindow = true
        NSApp.activate(ignoringOtherApps: true)
        // Post notification to show preferences
        NotificationCenter.default.post(name: NSNotification.Name("ShowPreferences"), object: nil)
    }

    @objc func toggleLaunchAtLogin() {
        if isLaunchAtLoginEnabled() {
            disableLaunchAtLogin()
        } else {
            enableLaunchAtLogin()
        }
        updateMenu()
    }

    @objc func quit() {
        NSApp.terminate(nil)
    }

    // MARK: - Launch at Login (SMAppService, macOS 13+)

    func isLaunchAtLoginEnabled() -> Bool {
        return SMAppService.mainApp.status == .enabled
    }

    func enableLaunchAtLogin() {
        do {
            try SMAppService.mainApp.register()
        } catch {
            print("Failed to enable launch at login: \(error)")
        }
    }

    func disableLaunchAtLogin() {
        do {
            try SMAppService.mainApp.unregister()
        } catch {
            print("Failed to disable launch at login: \(error)")
        }
    }
}
