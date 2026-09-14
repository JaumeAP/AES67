//
// PrivilegedScriptTests.swift
// AES67 Manager
//
// The first tests this app has. They cover the one piece of it that is pure
// text and can go wrong silently: the privileged command
// (Models/PrivilegedScript.swift).
//
// Not XCTest: the app is compiled by build.sh with plain swiftc,
// not SwiftPM, and a test bundle would be a second build system for six
// functions. This is a binary that returns non-zero when something fails, in
// the same shape as the Teensy package's host tests.
//

import Foundation

var failures = 0
var checks = 0

func check(_ condition: Bool, _ what: String, file: StaticString = #file, line: UInt = #line) {
    checks += 1
    if !condition {
        failures += 1
        FileHandle.standardError.write("FAIL \(file):\(line): \(what)\n".data(using: .utf8)!)
    }
}

func checkEqual(_ actual: String?, _ expected: String?, _ what: String,
                file: StaticString = #file, line: UInt = #line) {
    checks += 1
    if actual != expected {
        failures += 1
        let message = "FAIL \(file):\(line): \(what)\n  expected: \(expected ?? "nil")\n"
                    + "  actual:   \(actual ?? "nil")\n"
        FileHandle.standardError.write(message.data(using: .utf8)!)
    }
}

/// Every check in this file. Called from main.swift.
func runPrivilegedScriptTests() {
    // MARK: - shellQuoted

    checkEqual(PrivilegedScript.shellQuoted("/Applications/AES67Manager.app"),
               "'/Applications/AES67Manager.app'",
               "an ordinary path is simply single-quoted")

    checkEqual(PrivilegedScript.shellQuoted("/Users/who/Jaume's Mac/App.app"),
               "'/Users/who/Jaume'\\''s Mac/App.app'",
               "an apostrophe closes, escapes and reopens the quoting")

    checkEqual(PrivilegedScript.shellQuoted("/tmp/a b"), "'/tmp/a b'",
               "a space needs nothing beyond the quotes")

    // A path chosen to break out of the command if it were interpolated raw.
    // Checked by asking a real shell what it makes of the quoted form: the
    // only honest test of quoting is that the shell hands back exactly the
    // string that went in, as one word.
    for hostile in ["/tmp/x'; rm -rf /; echo '",
                    "/tmp/a b",
                    "/tmp/it's a \"path\"",
                    "/tmp/$HOME`whoami`"] {
        let quoted = PrivilegedScript.shellQuoted(hostile)
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/bin/sh")
        process.arguments = ["-c", "printf %s \(quoted)"]
        let pipe = Pipe()
        process.standardOutput = pipe
        do {
            try process.run()
            let data = pipe.fileHandleForReading.readDataToEndOfFile()
            process.waitUntilExit()
            let back = String(data: data, encoding: .utf8)
            checkEqual(back, hostile, "the shell reads back exactly what was quoted")
        } catch {
            check(false, "could not run /bin/sh: \(error)")
        }
    }

    // MARK: - appleScriptEscaped

    checkEqual(PrivilegedScript.appleScriptEscaped("say \"hi\""),
               "say \\\"hi\\\"",
               "a double quote is escaped for the AppleScript literal")

    checkEqual(PrivilegedScript.appleScriptEscaped("a\\b"), "a\\\\b",
               "a backslash is escaped, and before the quotes are")

    checkEqual(PrivilegedScript.appleScriptEscaped("\\\""), "\\\\\\\"",
               "backslash then quote escapes in that order, not the other way")

    // MARK: - isCarryable

    check(PrivilegedScript.isCarryable(["echo hi"]), "an ordinary command is carryable")
    check(!PrivilegedScript.isCarryable(["printf '{\n  \"active\": false\n}'"]),
          "a command containing a newline is not carryable")
    check(!PrivilegedScript.isCarryable(["echo a\r"]), "a carriage return is not carryable either")

    // MARK: - adminShell

    checkEqual(PrivilegedScript.adminShell(["chmod 644 '/tmp/x'"]),
               "do shell script \"chmod 644 '/tmp/x'\" with administrator privileges",
               "one command becomes one do shell script")

    checkEqual(PrivilegedScript.adminShell(["a", "b", "c"]),
               "do shell script \"a && b && c\" with administrator privileges",
               "commands are chained with && so the first failure stops the rest")

    checkEqual(PrivilegedScript.adminShell([]), nil, "no commands, no script")

    // This is the defect the file exists to prevent: JSON put into the command.
    let json = "{\n  \"active\": false\n}"
    checkEqual(PrivilegedScript.adminShell(["printf '%s' '\(json)' > '/tmp/f'"]), nil,
               "a command carrying JSON is refused rather than producing a broken script")

    // And the same content is fine once it is in a file the command copies.
    let viaFile = PrivilegedScript.adminShell([
        "/bin/cp \(PrivilegedScript.shellQuoted("/tmp/staged.json")) "
        + "\(PrivilegedScript.shellQuoted("/Library/Application Support/AES67Driver/device_active.json"))",
    ])
    check(viaFile != nil, "copying a staged file is carryable")
    check(viaFile?.contains("\n") == false, "the script it builds is a single line")

    // The result must be a well-formed AppleScript literal: exactly two unescaped
    // double quotes, the ones that open and close it.
    if let script = viaFile {
        var unescaped = 0
        var previous: Character = " "
        for character in script {
            if character == "\"" && previous != "\\" { unescaped += 1 }
            previous = character
        }
        check(unescaped == 2, "the literal has exactly two unescaped quotes, found \(unescaped)")
    }


}

// MARK: - The uninstaller's command
//
// This runs as root with one administrator prompt, so what it contains is
// worth pinning: the right paths, in an order that leaves the machine
// consistent, and nothing that could not be carried through AppleScript.

func runUninstallPlanTests() {
    let plain = UninstallPlan.commands(removingApplications: false)

    check(plain.contains { $0.contains("/Library/Audio/Plug-Ins/HAL/AES67Driver.driver") },
          "the driver bundle is removed")
    check(plain.contains { $0.contains("/Library/Application Support/AES67Driver") },
          "and the settings the driver reads as root")
    check(plain.contains { $0.contains("launchctl bootout system/com.aes67driver.ptpd") },
          "the PTP daemon is stopped")
    check(plain.first?.contains("bootout") == true,
          "the daemon goes first, while what it needs is still there")
    check(plain.last?.contains("killall coreaudiod") == true,
          "and Core Audio is restarted last, or the device stays until a reboot")

    // A job that was never registered must not stop the rest.
    check(plain.contains { $0.contains("bootout") && $0.contains("|| true") },
          "an absent daemon is not a failure")

    // The applications only when asked.
    check(!plain.contains { $0.contains("/Applications/AES67Manager.app") },
          "the apps are left alone by default")
    let withApps = UninstallPlan.commands(removingApplications: true)
    check(withApps.contains { $0.contains("/Applications/AES67Manager.app") },
          "and removed when asked")
    check(withApps.contains { $0.contains("/Applications/AES67Controller.app") },
          "both of them")
    check(withApps.last?.contains("killall coreaudiod") == true, "Core Audio still last")

    // Every path is quoted, and the whole thing survives the trip through
    // AppleScript.
    check(UninstallPlan.script(removingApplications: true) != nil,
          "the script can be built")
    check(PrivilegedScript.isCarryable(withApps), "and carried")
    check(UninstallPlan.script(removingApplications: false)?
            .hasPrefix("do shell script \"") == true,
          "as one administrator prompt")

    // The per-user settings are not in the privileged half: deleting a file in
    // the user's own home does not need a password.
    check(!plain.contains { $0.contains("Library/Application Support/AES67Driver") &&
                            $0.contains("~") },
          "the user's own copy is removed without privileges")
}
