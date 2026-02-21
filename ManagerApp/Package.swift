// swift-tools-version: 5.9
// AES67 Manager App Package

import PackageDescription

let package = Package(
    name: "AES67Manager",
    platforms: [
        .macOS(.v13)
    ],
    products: [
        .executable(
            name: "AES67Manager",
            targets: ["AES67Manager"]
        )
    ],
    targets: [
        .executableTarget(
            name: "AES67Manager",
            path: ".",
            sources: [
                "AES67ManagerApp.swift",
                "Views/ContentView.swift",
                "Views/StreamListView.swift",
                "Views/StreamDetailView.swift",
                "Views/AddStreamView.swift",
                "Views/SettingsView.swift",
                "Views/AudioStatusView.swift",
                "Views/QuickStartView.swift",
                "Views/PTPDiagnosticView.swift",
                "Views/ChannelMappingView.swift",
                "Views/ChannelMapDiagnosticView.swift",
                "Models/StreamInfo.swift",
                "Models/DriverManager.swift",
                "Models/MenuBarManager.swift"
            ]
        )
    ]
)
