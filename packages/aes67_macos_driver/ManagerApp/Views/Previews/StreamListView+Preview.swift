//
// StreamListView+Preview.swift
// AES67 Manager
// Xcode canvas previews, kept out of the view file itself.
//
// ManagerApp/build.sh lists its sources explicitly and does not include
// this directory: the #Preview macro needs the PreviewsMacros plugin,
// which ships with full Xcode and not with the Command Line Tools, so a
// command-line build would fail on it. Open the project in Xcode and add
// these files to the target to get the canvas back.
//

import SwiftUI

#Preview {
    StreamListView(selectedStream: .constant(nil))
        .environmentObject(DriverManager())
}
