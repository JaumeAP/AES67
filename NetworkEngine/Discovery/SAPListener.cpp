#include "SAPListener.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <thread>
#include <iostream>
#include <vector>
#include <algorithm>

namespace AES67 {

// PIMPL idiom to hide platform-specific implementation details
class SAPListener::Impl {
public:
    Impl() : running_(false), sockFd_(-1) {
    }
    
    ~Impl() {
        stop();
    }
    
    bool initialize() {
        // Create UDP socket
        sockFd_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (sockFd_ < 0) {
            std::cerr << "Failed to create SAP socket" << std::endl;
            return false;
        }
        
        // Enable SO_REUSEADDR to allow reusing the port
        int opt = 1;
        if (setsockopt(sockFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            std::cerr << "Failed to set socket options" << std::endl;
            close(sockFd_);
            return false;
        }
        
        // Bind to SAP multicast address
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(9875);  // SAP port
        addr.sin_addr.s_addr = inet_addr("224.2.127.254");  // SAP multicast address
        
        if (bind(sockFd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "Failed to bind SAP socket" << std::endl;
            close(sockFd_);
            return false;
        }
        
        // Join multicast group
        struct ip_mreq mreq;
        mreq.imr_multiaddr.s_addr = inet_addr("224.2.127.254");
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        
        if (setsockopt(sockFd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
            std::cerr << "Failed to join SAP multicast group" << std::endl;
            close(sockFd_);
            return false;
        }
        
        return true;
    }
    
    bool start() {
        if (running_) {
            return false;
        }
        
        running_ = true;
        listenThread_ = std::thread(&Impl::listenLoop, this);
        
        return true;
    }
    
    void stop() {
        if (!running_) {
            return;
        }
        
        running_ = false;
        
        if (listenThread_.joinable()) {
            listenThread_.join();
        }
        
        if (sockFd_ >= 0) {
            close(sockFd_);
            sockFd_ = -1;
        }
    }
    
    void registerAnnouncementCallback(const SAPAnnouncementCallback& callback) {
        std::lock_guard<std::mutex> lock(callbacksMutex_);
        callbacks_.push_back(callback);
    }
    
    std::vector<SAPAnnouncement> getDiscoveredStreams() const {
        std::lock_guard<std::mutex> lock(discoveredStreamsMutex_);
        return discoveredStreams_;
    }
    
private:
    void listenLoop() {
        char buffer[2048];
        
        while (running_) {
            struct sockaddr_in srcAddr;
            socklen_t addrLen = sizeof(srcAddr);
            
            ssize_t bytesRead = recvfrom(sockFd_, buffer, sizeof(buffer)-1, 0,
                                        (struct sockaddr*)&srcAddr, &addrLen);
            
            if (bytesRead > 0) {
                buffer[bytesRead] = '\0';
                
                // Parse the SAP announcement
                SAPAnnouncement announcement = parseSAPAnnouncement(buffer, bytesRead, 
                                                                   inet_ntoa(srcAddr.sin_addr));
                
                if (!announcement.sessionDescription.empty()) {
                    // Store the announcement
                    {
                        std::lock_guard<std::mutex> lock(discoveredStreamsMutex_);
                        
                        // Check if we already have this announcement to avoid duplicates
                        bool found = false;
                        for (const auto& existing : discoveredStreams_) {
                            if (existing.sessionName == announcement.sessionName && 
                                existing.sourceAddress == announcement.sourceAddress) {
                                found = true;
                                break;
                            }
                        }
                        
                        if (!found) {
                            discoveredStreams_.push_back(announcement);
                            
                            // Keep only the most recent announcements (e.g., last 50)
                            if (discoveredStreams_.size() > 50) {
                                discoveredStreams_.erase(discoveredStreams_.begin());
                            }
                        }
                    }
                    
                    // Notify all registered callbacks
                    {
                        std::lock_guard<std::mutex> lock(callbacksMutex_);
                        for (const auto& callback : callbacks_) {
                            callback(announcement);
                        }
                    }
                }
            }
        }
    }
    
    SAPAnnouncement parseSAPAnnouncement(const char* data, size_t length, 
                                       const std::string& sourceAddress) {
        SAPAnnouncement announcement;
        announcement.sourceAddress = sourceAddress;
        
        // SAP header is 4 bytes:
        // Bits 0-7: Version (3 bits), Type (1 bit), Encrypted (1 bit), Compressed (1 bit), Auth Length (2 bits)
        // Bits 8-31: Message ID Hash
        // Bits 32-47: Length
        // Bits 48-63: Source Address Type
        
        if (length < 4) {
            return announcement; // Not enough data for SAP header
        }
        
        // Check if this is a SAP announcement (type bit should be 0 for announcement)
        uint8_t sapHeader = static_cast<uint8_t>(data[0]);
        uint8_t typeBit = (sapHeader >> 3) & 0x01;
        
        if (typeBit != 0) {
            return announcement; // Not an announcement (might be deletion)
        }
        
        // The payload typically starts after 4 bytes (or 8 if extended header is present)
        // For simplicity, we'll assume basic header (4 bytes)
        size_t payloadStart = 4;
        if (length <= payloadStart) {
            return announcement; // No payload
        }
        
        // The payload should be an SDP description
        std::string sdpContent(data + payloadStart, length - payloadStart);
        announcement.sessionDescription = sdpContent;
        
        // Parse basic SDP information
        parseSDPInfo(sdpContent, announcement);
        
        return announcement;
    }
    
    void parseSDPInfo(const std::string& sdp, SAPAnnouncement& announcement) {
        // Parse the SDP content to extract stream information
        size_t lastPos = 0;
        size_t pos = 0;

        while ((pos = sdp.find('\n', lastPos)) != std::string::npos) {
            std::string line = sdp.substr(lastPos, pos - lastPos);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back(); // Remove carriage return if present
            }

            // Parse session name
            if (line.length() >= 2 && line.substr(0, 2) == "s=") {
                announcement.sessionName = line.substr(2);
            }
            // Parse connection information
            else if (line.length() >= 2 && line.substr(0, 2) == "c=") {
                // Format: c=IN IP4 <address>
                size_t addrStart = line.rfind(' ');
                if (addrStart != std::string::npos) {
                    announcement.multicastAddress = line.substr(addrStart + 1);
                }
            }
            // Parse media information
            else if (line.length() >= 2 && line.substr(0, 2) == "m=") {
                // Format: m=audio <port> RTP/AVP <payload_type>
                size_t portStart = line.find(' ', 2);
                if (portStart != std::string::npos) {
                    portStart++; // Skip the space
                    size_t portEnd = line.find(' ', portStart);
                    if (portEnd != std::string::npos) {
                        std::string portStr = line.substr(portStart, portEnd - portStart);
                        try {
                            announcement.port = std::stoi(portStr);
                        } catch (...) {
                            announcement.port = 0;
                        }
                    }
                }
            }
            // Parse RTP attribute (a=rtpmap)
            else if (line.length() >= 9 && line.substr(0, 9) == "a=rtpmap:") {
                // Format: a=rtpmap:<payload_type> <encoding_name>/<clock_rate>[/<channels>]
                // This can be used for additional stream information if needed
            }

            lastPos = pos + 1;
        }

        // Handle the final line without newline
        if (lastPos < sdp.length()) {
            std::string line = sdp.substr(lastPos);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            // Parse session name
            if (line.length() >= 2 && line.substr(0, 2) == "s=") {
                announcement.sessionName = line.substr(2);
            }
            // Parse connection information
            else if (line.length() >= 2 && line.substr(0, 2) == "c=") {
                size_t addrStart = line.rfind(' ');
                if (addrStart != std::string::npos) {
                    announcement.multicastAddress = line.substr(addrStart + 1);
                }
            }
            // Parse media information
            else if (line.length() >= 2 && line.substr(0, 2) == "m=") {
                size_t portStart = line.find(' ', 2);
                if (portStart != std::string::npos) {
                    portStart++; // Skip the space
                    size_t portEnd = line.find(' ', portStart);
                    if (portEnd != std::string::npos) {
                        std::string portStr = line.substr(portStart, portEnd - portStart);
                        try {
                            announcement.port = std::stoi(portStr);
                        } catch (...) {
                            announcement.port = 0;
                        }
                    }
                }
            }
        }
    }
    
    std::atomic<bool> running_;
    int sockFd_;
    std::thread listenThread_;
    
    mutable std::mutex callbacksMutex_;
    std::vector<SAPAnnouncementCallback> callbacks_;
    
    mutable std::mutex discoveredStreamsMutex_;
    std::vector<SAPAnnouncement> discoveredStreams_;
};

SAPListener::SAPListener() : pimpl_(std::make_unique<Impl>()) {
}

SAPListener::~SAPListener() = default;

bool SAPListener::initialize() {
    return pimpl_->initialize();
}

bool SAPListener::start() {
    return pimpl_->start();
}

void SAPListener::stop() {
    pimpl_->stop();
}

void SAPListener::registerAnnouncementCallback(const SAPAnnouncementCallback& callback) {
    pimpl_->registerAnnouncementCallback(callback);
}

std::vector<SAPAnnouncement> SAPListener::getDiscoveredStreams() const {
    return pimpl_->getDiscoveredStreams();
}

} // namespace AES67