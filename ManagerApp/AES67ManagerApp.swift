//
// AES67ManagerApp.swift
// AES67 Manager - Build #18
// SwiftUI application for managing AES67 audio streams
// With menu bar integration and first-run wizard
//

import SwiftUI
import AppKit

@main
struct AES67ManagerApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var appDelegate
    @StateObject private var driverManager = DriverManager()
    @StateObject private var menuBarManager: MenuBarManager
    @AppStorage("hasCompletedQuickStart") private var hasCompletedQuickStart = false
    @State private var showQuickStart = false

    init() {
        let driverMgr = DriverManager()
        _driverManager = StateObject(wrappedValue: driverMgr)
        _menuBarManager = StateObject(wrappedValue: MenuBarManager(driverManager: driverMgr))
    }

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(driverManager)
                .environmentObject(menuBarManager)
                .frame(minWidth: 900, minHeight: 600)
                .onAppear {
                    // Show quick start wizard on first run
                    if !hasCompletedQuickStart {
                        showQuickStart = true
                    }
                }
                .sheet(isPresented: $showQuickStart) {
                    QuickStartView(isPresented: $showQuickStart)
                        .environmentObject(driverManager)
                }
                .onReceive(NotificationCenter.default.publisher(for: NSNotification.Name("ShowAddStream"))) { _ in
                    driverManager.showAddStreamSheet = true
                }
                .onReceive(NotificationCenter.default.publisher(for: NSNotification.Name("ShowPreferences"))) { _ in
                    NSApp.sendAction(Selector(("showPreferencesWindow:")), to: nil, from: nil)
                }
                .onReceive(NotificationCenter.default.publisher(for: NSNotification.Name("ShowQuickStart"))) { _ in
                    showQuickStart = true
                }
        }
        .windowStyle(.hiddenTitleBar)
        .windowResizability(.contentSize)
        .commands {
            CommandGroup(replacing: .newItem) {
                Button("Import SDP File...") {
                    driverManager.importSDPFile()
                }
                .keyboardShortcut("o", modifiers: .command)
            }

            CommandGroup(after: .newItem) {
                Button("Add Stream Manually...") {
                    driverManager.showAddStreamSheet = true
                }
                .keyboardShortcut("n", modifiers: .command)

                Divider()

                Button("Refresh Status") {
                    driverManager.refreshStatus()
                }
                .keyboardShortcut("r", modifiers: .command)
            }

            CommandGroup(replacing: .help) {
                Button("Quick Start Guide...") {
                    NotificationCenter.default.post(name: NSNotification.Name("ShowQuickStart"), object: nil)
                }

                Divider()

                Button("AES67 Manager Help") {
                    if let url = URL(string: "https://github.com/yourusername/AES67_macos_Driver#readme") {
                        NSWorkspace.shared.open(url)
                    }
                }
            }
        }

        Settings {
            SettingsView()
                .environmentObject(driverManager)
        }
    }
}

// AppDelegate to handle menu bar and app lifecycle
class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        // Hide dock icon if user prefers menu bar only mode
        // Comment this out to keep dock icon visible
        // NSApp.setActivationPolicy(.accessory)
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        // Keep app running when window is closed (menu bar mode)
        return false
    }
}
