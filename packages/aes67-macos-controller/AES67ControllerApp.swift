//
// AES67ControllerApp.swift
// AES67 Controller
//
// A controller, not a manager: this application is about the devices on the
// network, not about this machine's audio driver. It installs nothing, it
// needs no driver present, and it touches no Core Audio.
//

import SwiftUI

@main
struct AES67ControllerApp: App {
    var body: some Scene {
        WindowGroup("AES67 Controller") {
            ControllerWindow()
        }
        .commands {
            CommandGroup(replacing: .newItem) {}
        }
    }
}
