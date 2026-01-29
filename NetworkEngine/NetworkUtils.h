#ifndef NETWORK_UTILS_H
#define NETWORK_UTILS_H

#include <string>
#include <vector>

namespace AES67 {

/**
 * Utility functions for network configuration specific to AES67
 */
class NetworkUtils {
public:
    /**
     * Join a multicast group on a specific interface
     * @param sockfd Socket file descriptor
     * @param multicastAddr Multicast address to join
     * @param interfaceName Name of the network interface (e.g., "en0")
     * @return True on success, false on failure
     */
    static bool joinMulticastGroup(int sockfd, const std::string& multicastAddr, 
                                   const std::string& interfaceName);
    
    /**
     * Leave a multicast group
     * @param sockfd Socket file descriptor
     * @param multicastAddr Multicast address to leave
     * @param interfaceName Name of the network interface
     * @return True on success, false on failure
     */
    static bool leaveMulticastGroup(int sockfd, const std::string& multicastAddr, 
                                    const std::string& interfaceName);
    
    /**
     * Set QoS/DSCP markings for AES67 traffic
     * @param sockfd Socket file descriptor
     * @param dscpValue DSCP value to set (e.g., 46 for EF, 34 for AF41)
     * @return True on success, false on failure
     */
    static bool setQoSTrafficClass(int sockfd, int dscpValue);
    
    /**
     * Bind socket to a specific network interface
     * @param sockfd Socket file descriptor
     * @param interfaceName Name of the network interface
     * @return True on success, false on failure
     */
    static bool bindToInterface(int sockfd, const std::string& interfaceName);
    
    /**
     * Get available network interfaces
     * @return Vector of interface names
     */
    static std::vector<std::string> getNetworkInterfaces();
    
    /**
     * Find the primary ethernet interface
     * @return Name of the primary ethernet interface, or empty string if not found
     */
    static std::string getPrimaryEthernetInterface();
    
    /**
     * Validate multicast address
     * @param addr IP address string
     * @return True if valid multicast address, false otherwise
     */
    static bool isValidMulticastAddress(const std::string& addr);

    /**
     * Check if multicast routing is configured for 239.x.x.x range
     * @param interfaceName Optional interface name to check (e.g., "en0")
     * @return True if multicast route exists, false otherwise
     */
    static bool hasMulticastRoute(const std::string& interfaceName = "");

    /**
     * Get recommended command to fix multicast routing
     * @param interfaceName Interface name to use (e.g., "en0")
     * @return Shell command string to add multicast route
     */
    static std::string getMulticastRouteCommand(const std::string& interfaceName);
};

} // namespace AES67

#endif // NETWORK_UTILS_H