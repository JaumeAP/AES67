//
// PrivilegedScriptTests.swift
// AES67 Manager
//
// The first tests this app has. They cover the one piece of it that is pure
// text and can go wrong silently: the privileged command
// (Models/PrivilegedScript.swift).
//
// Not XCTest: the app is compiled by ManagerApp/build.sh with plain swiftc,
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
