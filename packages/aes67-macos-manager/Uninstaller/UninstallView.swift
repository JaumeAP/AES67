//
// UninstallView.swift
// AES67 Uninstall
//

import AppKit
import SwiftUI

struct UninstallView: View {
    @State private var removeApplications = false
    @State private var result: String?
    @State private var failed = false
    @State private var working = false

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("Remove AES67 from this Mac")
                .font(.title3)
                .fontWeight(.semibold)

            VStack(alignment: .leading, spacing: 6) {
                Text("This removes:").font(.callout)
                ForEach(UninstallPlan.systemItems, id: \.self) { item in
                    Text("• \(item)").font(.caption).foregroundColor(.secondary)
                }
                Text("• \(UninstallPlan.userSupportPath) (this user's settings)")
                    .font(.caption).foregroundColor(.secondary)
            }

            Toggle("Also remove AES67Manager.app and AES67Controller.app from Applications",
                   isOn: $removeApplications)
                .font(.callout)

            Text("Core Audio is restarted at the end, which is what makes the device disappear. "
               + "An administrator password is asked for once.")
                .font(.caption)
                .foregroundColor(.secondary)
                .fixedSize(horizontal: false, vertical: true)

            if let result {
                Text(result)
                    .font(.callout)
                    .foregroundColor(failed ? .red : .green)
                    .fixedSize(horizontal: false, vertical: true)
            }

            HStack {
                Spacer()
                Button("Quit") { NSApplication.shared.terminate(nil) }
                Button(working ? "Removing…" : "Remove") { remove() }
                    .keyboardShortcut(.defaultAction)
                    .disabled(working)
            }
        }
        .padding(20)
        .frame(width: 520)
    }

    private func remove() {
        working = true
        result = nil

        // The per-user copy needs no privileges, and asking for them to delete
        // a file in the user's own home would be theatre.
        try? FileManager.default.removeItem(atPath: UninstallPlan.expandedUserSupportPath)

        guard let source = UninstallPlan.script(removingApplications: removeApplications) else {
            failed = true
            result = "The uninstall command could not be built. That is a defect, not something "
                   + "to retry."
            working = false
            return
        }

        var error: NSDictionary?
        NSAppleScript(source: source)?.executeAndReturnError(&error)
        working = false

        if let error {
            failed = true
            result = "Could not finish: \(error[NSAppleScript.errorMessage] ?? "unknown error"). "
                   + "Nothing may have been removed."
            return
        }
        failed = false
        result = "Removed. Core Audio has been restarted, so the AES67 device is gone from the "
               + "system's audio devices."
    }
}
