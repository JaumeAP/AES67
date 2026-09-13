//
// NetworkInterfaces.h
// AES67 RAVENNA session layer
// What the host's own interfaces are called and what their addresses are.
//
// IS-04 makes a node publish its interfaces and makes every sender and
// receiver name the one it uses, and the name alone is not enough: the
// specification identifies a port by its MAC. Both the announcer tool and the
// macOS driver need the same answer out of the same getifaddrs() walk, and
// each had been going to write its own.
//
// Not in aes67-core: <ifaddrs.h> is a system header, and the core's contract
// is that nothing reachable from it includes one.
//
#pragma once

#include <optional>
#include <string>

namespace AES67::Ravenna {

/// The hardware address of a named interface, written the way IS-04 writes
/// one: six lower-case hex pairs joined by hyphens, as its own schema
/// requires. Nothing when there is no such interface or it has no hardware
/// address of its own -- a loopback has none, and a node on one publishes no
/// port id rather than a made-up one.
std::optional<std::string> macAddressOf(const std::string& interfaceName);

}  // namespace AES67::Ravenna
