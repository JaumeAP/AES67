#!/bin/bash
#
# run-tests.sh
# AES67 Manager
# Host tests for the parts of the app that are testable without a Mac in any
# particular state: plain Swift over plain values, no SwiftUI, no Core Audio,
# no driver, no password prompt.
#
# Compiled with swiftc, like the app itself (ManagerApp/build.sh), rather than
# through SwiftPM and XCTest: the app is not a SwiftPM target and a test bundle
# would mean a second build system for a handful of functions.
#
# CTest runs this as `ManagerAppUnit`. Run it directly while working.
#
set -euo pipefail

cd "$(cd "$(dirname "$0")" && pwd)"

BIN="${AES67_BUILD_DIR:-.}/ManagerAppTests"
mkdir -p "$(dirname "$BIN")"

swiftc -o "$BIN" \
    -swift-version 5 \
    -warnings-as-errors \
    Models/PrivilegedScript.swift \
    Tests/PrivilegedScriptTests.swift \
    Tests/main.swift

"$BIN"
