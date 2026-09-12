//
// InstallerView.swift
// AES67 Install
//
// One window, always the same shape: what can be on this machine, what is on
// it now, and one button that makes the second match the first.
//
// A tick that is not there installs; a tick taken away removes that one
// application and nothing else. Below, separately, the button that takes
// everything off -- both applications, the driver, the PTP daemon and the
// settings -- because that is a different decision and should not be reached
// by unticking two boxes.
//

import AppKit
import SwiftUI

struct InstallerView: View {
    @State private var wanted: Set<InstallComponent> = Set(
        InstallComponent.allCases.filter { $0.installedByDefault })
    @State private var installed: Set<InstallComponent> = []
    @State private var message: String?
    @State private var failed = false
    @State private var working = false

    private var resourcesPath: String {
        Bundle.main.resourcePath ?? ""
    }

    private var pendingCommands: [String] {
        InstallPlan.applyCommands(wanted: wanted, installed: installed,
                                  resourcesPath: resourcesPath)
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            VStack(alignment: .leading, spacing: 3) {
                Text("AES67 for macOS").font(.title2).fontWeight(.semibold)
                Text("Choose what this machine should have. A tick installs; taking one away removes that application.")
                    .font(.caption)
                    .foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }

            ForEach(InstallComponent.allCases) { component in
                Toggle(isOn: binding(for: component)) {
                    VStack(alignment: .leading, spacing: 2) {
                        HStack(spacing: 6) {
                            Text(component.title)
                            if installed.contains(component) {
                                Text("installed").font(.caption2).foregroundColor(.secondary)
                            }
                        }
                        Text(component.summary)
                            .font(.caption)
                            .foregroundColor(.secondary)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
                .toggleStyle(.checkbox)
            }

            Text("The Manager carries the audio driver and the PTP daemon and puts them down itself, from its own window, once it is installed.")
                .font(.caption)
                .foregroundColor(.secondary)
                .fixedSize(horizontal: false, vertical: true)

            if let message {
                Text(message)
                    .font(.callout)
                    .foregroundColor(failed ? .red : .green)
                    .fixedSize(horizontal: false, vertical: true)
            }

            Divider()

            HStack {
                Button("Remove everything") { removeEverything() }
                    .disabled(working)
                    .help("Both applications, the driver, the PTP daemon and the settings.")
                Spacer()
                Button("Quit") { NSApplication.shared.terminate(nil) }
                Button(working ? "Working…" : "Apply") { apply() }
                    .keyboardShortcut(.defaultAction)
                    .disabled(working || pendingCommands.isEmpty)
            }
        }
        .padding(20)
        .frame(width: 560)
        .onAppear { readInstalled() }
    }

    private func binding(for component: InstallComponent) -> Binding<Bool> {
        Binding(get: { wanted.contains(component) },
                set: { on in
                    if on { wanted.insert(component) } else { wanted.remove(component) }
                })
    }

    /// What is actually in /Applications, which is what the ticks start as:
    /// a window that opens claiming to have installed nothing on a machine
    /// that has it all would remove it at the first Apply.
    private func readInstalled() {
        installed = Set(InstallComponent.allCases.filter {
            FileManager.default.fileExists(atPath: $0.installedPath)
        })
        if !installed.isEmpty { wanted = installed }
    }

    private func apply() {
        run(commands: pendingCommands,
            success: "Done. What is ticked is what is on this machine.")
    }

    private func removeEverything() {
        run(commands: InstallPlan.removeEverythingCommands(),
            success: "Removed: both applications, the driver, the PTP daemon and the settings. Core Audio has been restarted.")
        wanted = []
    }

    private func run(commands: [String], success: String) {
        guard let source = InstallPlan.script(commands) else {
            failed = true
            message = "Nothing to do."
            return
        }
        working = true
        var error: NSDictionary?
        NSAppleScript(source: source)?.executeAndReturnError(&error)
        working = false
        readInstalled()

        if let error {
            failed = true
            message = "Could not finish: \(error[NSAppleScript.errorMessage] ?? "unknown error")."
            return
        }
        failed = false
        message = success
    }
}
