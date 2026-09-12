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
/// `enable_sap` and `enable_rtsp` are 0/1. A caller that wants one of them
/// only pays for one of them: browsing mDNS registers with the system
/// responder, and joining the SAP groups is a multicast membership on the
/// machine.
AES67DiscoveryHandle* aes67_discovery_start(const char* interface_ip,
                                            int enable_sap, int enable_rtsp);

/// A snapshot of everything found, as a JSON array of objects with the keys
/// sessionName, sourceAddress, multicastAddress, port, ptpDomain, sdp and
/// sources (itself an array of "sap"/"rtsp"). Sessions that have stopped
/// being refreshed are swept as it reads, so what comes back is what is on
/// the network now.
///
/// Never NULL for a valid handle: an empty network is "[]". The caller owns
/// the string and frees it with aes67_discovery_free_string.
char* aes67_discovery_sessions_json(AES67DiscoveryHandle* handle);

void aes67_discovery_free_string(char* text);

/// Stops both discoverers and destroys the handle. NULL is a no-op.
void aes67_discovery_stop(AES67DiscoveryHandle* handle);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // AES67_DISCOVERY_BRIDGE_H
