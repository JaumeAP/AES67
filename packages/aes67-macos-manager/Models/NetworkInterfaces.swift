//
// NetworkInterfaces.swift
// AES67 Manager
// The machine's usable network interfaces, by name.
//
// Written out twice, identically, inside two views: SettingsView, where the
// user picks the interface the driver binds to, and QuickStartView, where the
// same list is shown while setting the driver up for the first time. Two
// copies of one list is two answers to "which interfaces are there", and the
// one the user configures with has to be the one the setup screen showed.
//

import Darwin
import Foundation

enum NetworkInterfaces {
    /// Every IPv4 interface the machine has, loopback left out, sorted and
    /// without repeats.
    ///
    /// Loopback is excluded because an AES67 stream bound to lo0 reaches
    /// nothing: the multicast group never leaves the machine.
    static func available() -> [String] {
        var interfaces: [String] = []
        var ifaddr: UnsafeMutablePointer<ifaddrs>?

        guard getifaddrs(&ifaddr) == 0 else { return interfaces }
        defer { freeifaddrs(ifaddr) }

        var ptr = ifaddr
        while ptr != nil {
            defer { ptr = ptr?.pointee.ifa_next }

            let interface = ptr!.pointee

            // An interface with no address bound -- a down utun or ppp
            // device, or one torn down between getifaddrs building the list
            // and this walk reaching it -- comes back with ifa_addr NULL.
            // The pointer is implicitly unwrapped, so dereferencing it
            // trapped, and taking the two copies of this walk down to one
            // made that a single crash both the Manager's settings screen and
            // its first-run check would take.
            guard let address = interface.ifa_addr else { continue }

            if address.pointee.sa_family == UInt8(AF_INET) {
                let name = String(cString: interface.ifa_name)
                if !name.hasPrefix("lo") && !interfaces.contains(name) {
                    interfaces.append(name)
                }
            }
        }

        return interfaces.sorted()
    }
}
