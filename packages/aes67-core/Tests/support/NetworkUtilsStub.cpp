//
// NetworkUtilsStub.cpp
// The seam, filled for tests -- and a worked example of filling it.
//
// StreamConfig calls three NetworkUtils functions that this library declares
// and cannot implement: the real ones open sockets and enumerate interfaces.
// A consumer supplies them (aes67_macos_driver does, in aes67_net; a firmware
// consumer would over lwIP), and so must the tests.
//
// Deliberately dumb and deterministic. A test double that went looking at the
// machine's real interfaces would make these suites depend on whichever laptop
// runs them, which is the opposite of what a test is for.
//

#include "NetworkEngine/NetworkUtils.h"

#include <cstdlib>
#include <utility>
#include <vector>

namespace AES67 {

bool NetworkUtils::isIPv4Address(const std::string& str) {
    // Four decimal fields, each 0-255. No hostnames, no IPv6: the real one is
    // stricter about neither.
    int fields = 0;
    size_t i = 0;
    while (i <= str.size()) {
        size_t start = i;
        while (i < str.size() && str[i] != '.') ++i;
        const std::string field = str.substr(start, i - start);
        if (field.empty() || field.size() > 3) return false;
        for (char c : field)
            if (c < '0' || c > '9') return false;
        const int value = std::atoi(field.c_str());
        if (value < 0 || value > 255) return false;
        ++fields;
        if (i == str.size()) break;
        ++i;
    }
    return fields == 4;
}

std::string NetworkUtils::getInterfaceIP(const std::string& interfaceName) {
    // One fixed answer for one fixed name, and nothing for anything else, so a
    // test asserting on the result is asserting on the code under test rather
    // than on the machine.
    if (interfaceName == "en0") return "192.0.2.10";
    return "";
}

std::vector<std::pair<std::string, std::string>> NetworkUtils::getActiveInterfacesWithIPs() {
    // One interface, always the same one. StreamConfig falls back to this when
    // no interface was configured, and a test that got the real list would pass
    // or fail depending on whether the machine had Wi-Fi on.
    return {{"en0", "192.0.2.10"}};
}

std::string NetworkUtils::resolveInterfaceToIP(const std::string& interfaceSpec) {
    if (isIPv4Address(interfaceSpec)) return interfaceSpec;
    return getInterfaceIP(interfaceSpec);
}

}  // namespace AES67
