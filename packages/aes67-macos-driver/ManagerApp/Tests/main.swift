//
// main.swift
// AES67 Manager
// Entry point for the host tests. Swift allows top-level code in this file
// only, so the checks live in their own files as functions and this runs them.
//

import Foundation

runPrivilegedScriptTests()

print("\(checks) checks, \(failures) failures")
exit(failures == 0 ? 0 : 1)
