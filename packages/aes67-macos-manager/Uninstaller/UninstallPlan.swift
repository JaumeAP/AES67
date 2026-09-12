//
// UninstallPlan.swift
// AES67 Uninstall
//
// What is on the machine, and the command that takes it off. Pure values and
// one string, so the host tests can check the command without running it --
// this is a script that runs as root, and the one thing it must never be is
// improvised.
//

import Foundation

enum UninstallPlan {
    /// The driver bundle Core Audio loads.
    static let driverPath = "/Library/Audio/Plug-Ins/HAL/AES67Driver.driver"
    /// Settings the driver reads from inside coreaudiod, whose HOME is not the
    /// logged-in user's: stream configuration, the activation flag, the
    /// discovery switch.
    static let systemSupportPath = "/Library/Application Support/AES67Driver"
    /// The same, per user, written by the app.
    static let userSupportPath = "~/Library/Application Support/AES67Driver"
    /// The PTP daemon's launchd label. Registered by the Manager through
    /// SMAppService from inside its own bundle, so another application cannot
    /// unregister it -- what it can do is stop the job and let the bundle's
    /// removal take the registration with it.
    static let daemonLabel = "com.aes67driver.ptpd"

    static let managerAppPath = "/Applications/AES67Manager.app"
    static let controllerAppPath = "/Applications/AES67Controller.app"

    static var expandedUserSupportPath: String {
        NSString(string: userSupportPath).expandingTildeInPath
    }

    /// What the window lists, in the order the script removes it.
    static let systemItems = [
        driverPath,
        "the PTP daemon (launchd job \(daemonLabel))",
        systemSupportPath,
    ]

    /// The privileged half. `|| true` on the daemon: a job that is not loaded
    /// is the normal case on a machine where the daemon was never registered,
    /// and it must not stop the rest.
    static func commands(removingApplications: Bool) -> [String] {
        let q = PrivilegedScript.shellQuoted
        var commands = [
            "launchctl bootout system/\(daemonLabel) || true",
            "rm -rf \(q(driverPath))",
            "rm -rf \(q(systemSupportPath))",
        ]
        if removingApplications {
            commands.append("rm -rf \(q(managerAppPath))")
            commands.append("rm -rf \(q(controllerAppPath))")
        }
        // Last, and always: until Core Audio is restarted the device is still
        // there, loaded from a bundle that no longer exists.
        commands.append("launchctl kickstart -kp system/com.apple.audio.coreaudiod")
        return commands
    }

    static func script(removingApplications: Bool) -> String? {
        PrivilegedScript.adminShell(commands(removingApplications: removingApplications))
    }
}
