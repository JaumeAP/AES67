#pragma once

// Network configuration of the box.
//
// It exists so that moving to another network does not mean touching
// main.cpp. The values used to come hardcoded from the library's example (a
// fixed IP 192.168.0.211 and a gateway 192.168.0.6 from the authors' lab),
// which matched no real installation.

// With 1 the box asks for an address over DHCP at start-up, and only falls
// back to the static configuration below if it gets none within the timeout.
// With 0 it goes straight to the static configuration.
#define AES67_USE_DHCP 1

// How long we wait for a DHCP address, in milliseconds, before giving up.
// This wait is only paid when DHCP really does fail.
#define AES67_DHCP_TIMEOUT_MS 15000

// Static configuration. Used if AES67_USE_DHCP is 0, or if DHCP does not
// answer.
//
// These values are a reasonable starting point, NOT a valid configuration for
// your network: if you mean to depend on this path, change them.
#define AES67_STATIC_IP      192, 168, 1, 211
#define AES67_STATIC_NETMASK 255, 255, 255, 0
#define AES67_STATIC_GATEWAY 192, 168, 1, 1

// The name the box gives the DHCP server.
#define AES67_HOSTNAME "aes67-master"

// The longest we wait for the USB serial port to open before carrying on, in
// milliseconds. Without it the first diagnostic lines are lost, and those are
// exactly the ones saying which network configuration was taken. It is capped
// so the box still starts with nobody watching.
#define AES67_SERIAL_WAIT_MS 2000
