//
// AES67UninstallApp.swift
// AES67 Uninstall
//
// The third thing in the disk image, and the one nobody thinks about until
// they need it.
//
// The Manager installs the driver and registers the PTP daemon, and it can
// remove them again -- while it is there. This exists for when it is not, or
// when somebody wants the machine clean without opening the thing they are
// removing: it puts down nothing, needs nothing installed, and knows only
// where the pieces live.
//

import SwiftUI

@main
struct AES67UninstallApp: App {
    var body: some Scene {
        WindowGroup("AES67 Uninstall") {
            UninstallView()
        }
        .windowResizability(.contentSize)
        .commands {
            CommandGroup(replacing: .newItem) {}
        }
    }
}
