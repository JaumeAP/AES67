//
// PrivilegedScript.swift
// AES67 Manager
// Building the one privileged command this app runs, as text.
//
// The app asks for an administrator password to put the driver in the HAL and
// to write the activation flag. That request goes through AppleScript, and an
// AppleScript `do shell script` takes its command as a string literal -- so
// every path and every value crosses two escaping layers: AppleScript's, and
// then the shell's. Getting either wrong does not fail loudly. It produces a
// script that does not compile, or worse, one that does something else.
//
// It is here, as free functions over strings and nothing else, so it can be
// tested without a driver, a password prompt or a Mac in any state:
// Tests/PrivilegedScriptTests.swift does exactly that.
//
// A defect this file exists to prevent, found by review after it shipped into
// a working tree: the activation flag was written by putting JSON inside the
// command. JSON is double quotes and newlines, an AppleScript string literal
// admits neither, and the switch could never have worked.
//

import Foundation

enum PrivilegedScript {
    /// A shell word: single-quoted, with any embedded single quote closed,
    /// escaped and reopened -- the only sequence that is safe inside single
    /// quotes, since a backslash means nothing to the shell in there.
    static func shellQuoted(_ value: String) -> String {
        "'" + value.replacingOccurrences(of: "'", with: "'\\''") + "'"
    }

    /// The same string as an AppleScript string literal's contents: backslash
    /// first (or it would escape the escapes that follow), then double quote.
    /// A newline cannot appear in an AppleScript literal at all, so it is not
    /// escaped here -- `adminShell` refuses a command carrying one.
    static func appleScriptEscaped(_ value: String) -> String {
        value
            .replacingOccurrences(of: "\\", with: "\\\\")
            .replacingOccurrences(of: "\"", with: "\\\"")
    }

    /// Whether these commands can be carried by an AppleScript literal.
    /// Newlines and carriage returns cannot; anything that needs them belongs
    /// in a file the command copies, not in the command.
    static func isCarryable(_ commands: [String]) -> Bool {
        !commands.contains { $0.contains("\n") || $0.contains("\r") }
    }

    /// The AppleScript that runs these commands, in order, stopping at the
    /// first failure, with one administrator prompt for the lot.
    ///
    /// Returns nil rather than a broken script when a command cannot be
    /// carried: a caller that ignores that gets no script rather than one that
    /// fails at compile time with a message about a stray quote.
    static func adminShell(_ commands: [String]) -> String? {
        guard !commands.isEmpty, isCarryable(commands) else { return nil }
        let joined = appleScriptEscaped(commands.joined(separator: " && "))
        return "do shell script \"\(joined)\" with administrator privileges"
    }
}
