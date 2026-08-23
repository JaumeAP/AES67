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
        .navigationTitle("AES67 Audio Driver")
        .toolbar {
            ToolbarItemGroup {
                Button(action: { driverManager.showAddStreamSheet = true }) {
                    Label("Add Stream", systemImage: "plus")
                }

                Button(action: { driverManager.importSDPFile() }) {
                    Label("Import SDP", systemImage: "doc.badge.plus")
                }

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
