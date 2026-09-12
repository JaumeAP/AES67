//
// AES67InstallApp.swift
// AES67 Install
//
// The program somebody runs, instead of a disk image somebody drags from.
//

import SwiftUI

@main
struct AES67InstallApp: App {
    var body: some Scene {
        WindowGroup("AES67 Install") {
            InstallerView()
        }
        .windowResizability(.contentSize)
        .commands {
            CommandGroup(replacing: .newItem) {}
        }
    }
}
