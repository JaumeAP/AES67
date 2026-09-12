//
// InstallPlan.swift
// AES67 Install
//
// What is on the machine, and the commands that put each piece there or take
// it away. Pure values and strings: this is what runs as root, so the host
// tests pin it rather than trusting a window.
//

import Foundation

/// One thing a person can choose to have or not have.
enum InstallComponent: String, CaseIterable, Identifiable {
    case manager
    case controller

    var id: String { rawValue }

    var appName: String {
        switch self {
        case .manager:    return "AES67Manager.app"
        case .controller: return "AES67Controller.app"
        }
    }

    var title: String {
        switch self {
        case .manager:    return "AES67 Manager"
        case .controller: return "AES67 Controller"
        }
    }

    var summary: String {
        switch self {
        case .manager:
            return "This machine: installs the audio driver and the PTP daemon, activates the device, picks the compatibility profile and maps channels."
        case .controller:
            return "The network: the sessions on it and the crosspoints between devices. Installs nothing and needs no driver."
        }
    }

    /// Ticked by default. The Manager is what a machine carrying audio needs;
    /// the Controller is for whoever routes the plant, which is not everyone.
    var installedByDefault: Bool { self == .manager }

    var installedPath: String { "/Applications/\(appName)" }
}

enum InstallPlan {
    /// Where the applications this installer carries live inside its own
    /// bundle.
    static func bundledPath(of component: InstallComponent, resourcesPath: String) -> String {
        resourcesPath + "/" + component.appName
    }

    /// Copying an application into /Applications, replacing whatever is there.
    ///
    /// `ditto` rather than `cp -R`: it is what preserves a bundle's extended
    /// attributes and its signature, and a driver bundle that loses either is
    /// a driver coreaudiod refuses to load.
    static func installCommands(_ component: InstallComponent, resourcesPath: String) -> [String] {
        let q = PrivilegedScript.shellQuoted
        let source = bundledPath(of: component, resourcesPath: resourcesPath)
        return [
            "rm -rf \(q(component.installedPath))",
            "ditto \(q(source)) \(q(component.installedPath))",
            "chown -R root:wheel \(q(component.installedPath))",
        ]
    }

    /// Removing one application, leaving the driver and the settings alone:
    /// unticking the Controller must not take the audio off a machine.
    static func removeCommands(_ component: InstallComponent) -> [String] {
        ["rm -rf \(PrivilegedScript.shellQuoted(component.installedPath))"]
    }

    /// What one press of Apply does: install what is ticked and is not there,
    /// remove what is unticked and is. Empty when the machine already matches
    /// what the window says, which is the case the button must not ask for a
    /// password in.
    static func applyCommands(wanted: Set<InstallComponent>,
                              installed: Set<InstallComponent>,
                              resourcesPath: String) -> [String] {
        var commands: [String] = []
        for component in InstallComponent.allCases {
            let isWanted = wanted.contains(component)
            let isInstalled = installed.contains(component)
            if isWanted && !isInstalled {
                commands += installCommands(component, resourcesPath: resourcesPath)
            } else if !isWanted && isInstalled {
                commands += removeCommands(component)
            }
        }
        return commands
    }

    /// Everything off the machine: both applications, the driver, the daemon
    /// and the settings. The same list the standalone uninstaller runs, which
    /// is the one place that knows it.
    static func removeEverythingCommands() -> [String] {
        // One list, and it lives with the uninstaller: two places that both
        // know what to delete is how one of them ends up deleting less.
        UninstallPlan.commands(removingApplications: true)
    }

    static func script(_ commands: [String]) -> String? {
        commands.isEmpty ? nil : PrivilegedScript.adminShell(commands)
    }
}
