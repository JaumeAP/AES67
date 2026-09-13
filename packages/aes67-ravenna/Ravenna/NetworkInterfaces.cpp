#include "Ravenna/NetworkInterfaces.h"

#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>
#if defined(__linux__)
#include <linux/if_packet.h>
#else
#include <net/if_dl.h>
#endif

#include <cstdio>

namespace AES67::Ravenna {

std::optional<std::string> macAddressOf(const std::string& interfaceName) {
    struct ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0) return std::nullopt;

    std::optional<std::string> found;
    for (const struct ifaddrs* entry = list; entry != nullptr; entry = entry->ifa_next) {
        if (entry->ifa_addr == nullptr || interfaceName != entry->ifa_name) continue;

        const unsigned char* bytes = nullptr;
        // The link-layer address is a separate entry from the IPv4 one, under
        // the same name, and the two families are spelled differently on the
        // two systems this builds for.
#if defined(__linux__)
        if (entry->ifa_addr->sa_family != AF_PACKET) continue;
        const auto* link = reinterpret_cast<const struct sockaddr_ll*>(entry->ifa_addr);
        if (link->sll_halen != 6) continue;
        bytes = link->sll_addr;
#else
        if (entry->ifa_addr->sa_family != AF_LINK) continue;
        const auto* link = reinterpret_cast<const struct sockaddr_dl*>(entry->ifa_addr);
        if (link->sdl_alen != 6) continue;
        bytes = reinterpret_cast<const unsigned char*>(LLADDR(link));
#endif
        char text[18];
        (void)std::snprintf(text, sizeof(text), "%02x-%02x-%02x-%02x-%02x-%02x", bytes[0],
                            bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
        found = std::string(text);
        break;
    }

    ::freeifaddrs(list);
    return found;
}

}  // namespace AES67::Ravenna
