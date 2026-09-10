#ifndef DRIVER_MANAGER_IPC_H
#define DRIVER_MANAGER_IPC_H

#include <string>
#include <functional>
#include <memory>

namespace AES67 {

// Structure to represent a stream configuration
struct StreamConfiguration {
    std::string multicastAddress;
    int port;
    int ptpDomain;
    int sampleRate;
    int channels;
    std::string streamName;
    
    StreamConfiguration() : port(0), ptpDomain(0), sampleRate(48000), channels(2) {}
};

// Callback type for receiving stream configurations
using StreamConfigCallback = std::function<void(const StreamConfiguration&)>;

class DriverManager {
public:
    DriverManager();
    ~DriverManager();
    
    // Initialize the IPC mechanism
    bool initialize();
    
    // Register a callback for receiving stream configurations
    void registerStreamConfigCallback(const StreamConfigCallback& callback);
    
    // Send a stream configuration to the driver
    bool sendStreamConfiguration(const StreamConfiguration& config);
    
    // Request current stream configurations from the driver
    bool requestStreamConfigurations();
    
    // Start the driver
    bool startDriver();
    
    // Stop the driver
    bool stopDriver();
    
private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace AES67

#endif // DRIVER_MANAGER_IPC_H