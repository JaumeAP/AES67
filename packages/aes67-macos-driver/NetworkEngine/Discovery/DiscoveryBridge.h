#ifndef AES67_DISCOVERY_BRIDGE_H
#define AES67_DISCOVERY_BRIDGE_H

//
// DiscoveryBridge
// AES67 macOS Driver
//
// The driver's discovery, reachable from outside the driver.
//
// SAPListener, MDNSBrowser and RTSPSessionDiscovery run inside coreaudiod and
// publish what they find through a Core Audio property, which means the list
// exists only where the plug-in is installed and active. A controller -- the
// crosspoint matrix, which is about other people's devices and needs no audio
// driver of its own -- has to be able to find sessions on a machine where this
// driver was never installed.
//
// So the same two discoverers are exposed here behind a C API, which is what a
// Swift application can link against: one handle, one snapshot of the session
// directory as JSON, one stop. JSON rather than a struct array because the
// lifetime of the strings is then the caller's problem for exactly as long as
// it holds the string, and because the reader on the other side already parses
// JSON.
//
// Nothing here touches audio, Core Audio, or the plug-in: this is the network
// half only.
//

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AES67DiscoveryHandle AES67DiscoveryHandle;

/// Starts discovery and returns a handle, or NULL when neither route could
/// start.
///
/// `interface_ip` is the local address to join multicast on -- the same
/// choice the driver makes from its configured interface. NULL or empty
/// leaves it to the kernel's routing table.
///
/// `enable_sap`, `enable_rtsp` and `enable_ptp` are 0/1. A caller that wants
/// one of them only pays for one of them: browsing mDNS registers with the
/// system responder, and joining the SAP or PTP groups is a multicast
/// membership on the machine.
///
/// `interface_name` selects the interface the PTP observer watches ("en0"),
/// which is the name and not the address -- that is what PTPPeerObserver
/// takes. NULL or empty lets the kernel choose.
AES67DiscoveryHandle* aes67_discovery_start(const char* interface_ip,
                                            const char* interface_name,
                                            int enable_sap, int enable_rtsp, int enable_ptp,
                                            int enable_services);

/// A snapshot of everything found, as a JSON array of objects with the keys
/// sessionName, sourceAddress, multicastAddress, port, ptpDomain, sdp and
/// sources (itself an array of "sap"/"rtsp"). Sessions that have stopped
/// being refreshed are swept as it reads, so what comes back is what is on
/// the network now.
///
/// Never NULL for a valid handle: an empty network is "[]". The caller owns
/// the string and frees it with aes67_discovery_free_string.
char* aes67_discovery_sessions_json(AES67DiscoveryHandle* handle);

/// The PTP participants seen on the network, as a JSON array with the keys
/// clockId, oui, role ("master"/"slave"/"mixed"/"unknown"), sourceIp, domain,
/// messageCount and secondsSinceLastSeen.
///
/// This is how gear that announces nothing is found at all. Dolby Atmos
/// Connect is configured by hand end to end -- no SAP, no registered service,
/// no NMOS -- so the only thing it puts on the network unasked is its clock:
/// a master peer is a processor feeding this system, a slave peer an
/// amplifier it feeds, and the clock identity carries the vendor's OUI. What
/// it cannot say is what a session IS, because nothing on the wire says so.
///
/// Empty array when the observer was not asked for or could not start.
char* aes67_discovery_ptp_peers_json(AES67DiscoveryHandle* handle);

/// Every service registered on the local link that this world uses, as a
/// JSON array with the keys name, type, host, address, port and
/// secondsSinceLastSeen.
///
/// Browsing is not control. A Dante device registers `_netaudio-arc._udp` and
/// its neighbours; reading that registration says a device of that kind is
/// here and what it calls itself, which is all a list needs, and none of it
/// touches the protocol Audinate licenses. Same for RAVENNA's `_rtsp._tcp`
/// and NMOS's `_nmos-node._tcp`: the point of the list is that a room shows
/// what is on it, whoever made it.
///
/// Empty array when service browsing was not asked for.
char* aes67_discovery_services_json(AES67DiscoveryHandle* handle);

void aes67_discovery_free_string(char* text);

/// Stops both discoverers and destroys the handle. NULL is a no-op.
void aes67_discovery_stop(AES67DiscoveryHandle* handle);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // AES67_DISCOVERY_BRIDGE_H
