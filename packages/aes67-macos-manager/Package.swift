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
            // `path: "."` hands SwiftPM the whole package directory, and the
            // three built app bundles live in it -- each carrying its own
            // copy of en/es/ca.lproj/Localizable.strings, so the manifest was
            // refused outright over duplicate resources before it could
            // describe anything. They are build products (.gitignore lines
            // 3-5) rather than sources, and so is build/; Installer/,
            // Uninstaller/ and Tests/ are compiled by their own scripts, not
            // by this target.
            exclude: [
                "AES67Manager.app",
                "AES67Install.app",
                "AES67Uninstall.app",
                "build",
                "Installer",
                "Uninstaller",
                "Tests",
                "scripts",
                "Localization",
                "Resources",
                "build.sh",
                "build-dmg.sh",
                "build-installer.sh",
                "build-uninstaller.sh",
                "run-tests.sh",
                "CMakeLists.txt",
                "DriverManager.cpp",
                "DriverManager.h",
                "ManagerAppTests",
                "README.md"
            ],
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
                "Views/ProfileParametersView.swift",
                "Views/DiscoveredSessionsView.swift",
                "Views/NoticeView.swift",
                "Views/ChannelMappingView.swift",
                "Views/ChannelMapDiagnosticView.swift",
                "Views/RoutingMatrixView.swift",
                "Models/StreamInfo.swift",
                "Models/DriverManager.swift",
                "Models/MenuBarManager.swift",
                "Models/NetworkInterfaces.swift",
                "Models/DolbyModelCatalog.swift",
                "Models/NmosResources.swift",
                "Models/NmosController.swift",
                "Models/SessionList.swift",
                "Models/PrivilegedScript.swift"
            ]
        )
    ]
)
