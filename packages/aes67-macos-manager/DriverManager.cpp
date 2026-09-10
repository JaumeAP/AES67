#include "DriverManager.h"
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <mutex>
#include <vector>

namespace AES67 {

// PIMPL idiom to hide platform-specific implementation details
class DriverManager::Impl {
public:
    Impl() : callback_(nullptr), queue_(dispatch_queue_create("aes67.driver.ipc", DISPATCH_QUEUE_SERIAL)) {
    }
    
    ~Impl() {
        if (notificationCenter_) {
            CFNotificationCenterRemoveObserver(notificationCenter_, this, 
                                              CFSTR("AES67.StreamConfigChanged"), nullptr);
        }
    }
    
    bool initialize() {
        notificationCenter_ = CFNotificationCenterGetDarwinNotifyCenter();
        if (!notificationCenter_) {
            return false;
        }
        
        // Register for notifications from the driver
        CFNotificationCenterAddObserver(
            notificationCenter_,
            this,  // observer
            &streamConfigChangedCallback,  // callback function
            "AES67.Driver.StreamConfigChanged",  // event name
            nullptr,  // object
            CFNotificationSuspensionBehaviorDeliverImmediately
        );
        
        return true;
    }
    
    void registerStreamConfigCallback(const StreamConfigCallback& callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        callback_ = callback;
    }
    
    bool sendStreamConfiguration(const StreamConfiguration& config) {
        // Serialize the configuration to a plist
        CFMutableDictionaryRef configDict = CFDictionaryCreateMutable(nullptr, 0, 
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        
        // Add configuration fields
        CFStringRef multicastAddr = CFStringCreateWithCString(nullptr, config.multicastAddress.c_str(), kCFStringEncodingUTF8);
        CFNumberRef port = CFNumberCreate(nullptr, kCFNumberIntType, &config.port);
        CFNumberRef domain = CFNumberCreate(nullptr, kCFNumberIntType, &config.ptpDomain);
        CFNumberRef sampleRate = CFNumberCreate(nullptr, kCFNumberIntType, &config.sampleRate);
        CFNumberRef channels = CFNumberCreate(nullptr, kCFNumberIntType, &config.channels);
        CFStringRef streamName = CFStringCreateWithCString(nullptr, config.streamName.c_str(), kCFStringEncodingUTF8);
        
        CFDictionarySetValue(configDict, CFSTR("MulticastAddress"), multicastAddr);
        CFDictionarySetValue(configDict, CFSTR("Port"), port);
        CFDictionarySetValue(configDict, CFSTR("PTPDomain"), domain);
        CFDictionarySetValue(configDict, CFSTR("SampleRate"), sampleRate);
        CFDictionarySetValue(configDict, CFSTR("Channels"), channels);
        CFDictionarySetValue(configDict, CFSTR("StreamName"), streamName);
        
        // Post the notification to the driver
        CFNotificationCenterPostNotification(
            notificationCenter_,
            CFSTR("AES67.Manager.StreamConfigChanged"),
            nullptr,  // object
            configDict,  // userInfo
            true  // deliver immediately
        );
        
        // Clean up
        CFRelease(multicastAddr);
        CFRelease(port);
        CFRelease(domain);
        CFRelease(sampleRate);
        CFRelease(channels);
        CFRelease(streamName);
        CFRelease(configDict);
        
        return true;
    }
    
    bool requestStreamConfigurations() {
        // Post a notification requesting stream configurations from the driver
        CFNotificationCenterPostNotification(
            notificationCenter_,
            CFSTR("AES67.Manager.RequestStreamConfigs"),
            nullptr,  // object
            nullptr,  // userInfo
            true  // deliver immediately
        );
        
        return true;
    }
    
    bool startDriver() {
        // Post a notification to start the driver
        CFNotificationCenterPostNotification(
            notificationCenter_,
            CFSTR("AES67.Manager.StartDriver"),
            nullptr,  // object
            nullptr,  // userInfo
            true  // deliver immediately
        );
        
        return true;
    }
    
    bool stopDriver() {
        // Post a notification to stop the driver
        CFNotificationCenterPostNotification(
            notificationCenter_,
            CFSTR("AES67.Manager.StopDriver"),
            nullptr,  // object
            nullptr,  // userInfo
            true  // deliver immediately
        );
        
        return true;
    }
    
    // Static callback for handling notifications from the driver
    static void streamConfigChangedCallback(CFNotificationCenterRef center, 
                                           void* observer,
                                           CFStringRef name,
                                           const void* object,
                                           CFDictionaryRef userInfo) {
        DriverManager::Impl* impl = static_cast<DriverManager::Impl*>(observer);
        if (impl && impl->callback_ && userInfo) {
            // Parse the configuration from the dictionary
            StreamConfiguration config;
            
            CFStringRef multicastAddrRef = (CFStringRef)CFDictionaryGetValue(userInfo, CFSTR("MulticastAddress"));
            if (multicastAddrRef) {
                char buffer[256];
                CFStringGetCString(multicastAddrRef, buffer, sizeof(buffer), kCFStringEncodingUTF8);
                config.multicastAddress = std::string(buffer);
            }
            
            CFNumberRef portRef = (CFNumberRef)CFDictionaryGetValue(userInfo, CFSTR("Port"));
            if (portRef) {
                int port;
                CFNumberGetValue(portRef, kCFNumberIntType, &port);
                config.port = port;
            }
            
            CFNumberRef domainRef = (CFNumberRef)CFDictionaryGetValue(userInfo, CFSTR("PTPDomain"));
            if (domainRef) {
                int domain;
                CFNumberGetValue(domainRef, kCFNumberIntType, &domain);
                config.ptpDomain = domain;
            }
            
            CFNumberRef sampleRateRef = (CFNumberRef)CFDictionaryGetValue(userInfo, CFSTR("SampleRate"));
            if (sampleRateRef) {
                int sampleRate;
                CFNumberGetValue(sampleRateRef, kCFNumberIntType, &sampleRate);
                config.sampleRate = sampleRate;
            }
            
            CFNumberRef channelsRef = (CFNumberRef)CFDictionaryGetValue(userInfo, CFSTR("Channels"));
            if (channelsRef) {
                int channels;
                CFNumberGetValue(channelsRef, kCFNumberIntType, &channels);
                config.channels = channels;
            }
            
            CFStringRef streamNameRef = (CFStringRef)CFDictionaryGetValue(userInfo, CFSTR("StreamName"));
            if (streamNameRef) {
                char buffer[256];
                CFStringGetCString(streamNameRef, buffer, sizeof(buffer), kCFStringEncodingUTF8);
                config.streamName = std::string(buffer);
            }
            
            // Call the registered callback
            impl->callback_(config);
        }
    }
    
private:
    CFNotificationCenterRef notificationCenter_;
    StreamConfigCallback callback_;
    std::mutex mutex_;
    dispatch_queue_t queue_;
};

DriverManager::DriverManager() : pimpl_(std::make_unique<Impl>()) {
}

DriverManager::~DriverManager() = default;

bool DriverManager::initialize() {
    return pimpl_->initialize();
}

void DriverManager::registerStreamConfigCallback(const StreamConfigCallback& callback) {
    pimpl_->registerStreamConfigCallback(callback);
}

bool DriverManager::sendStreamConfiguration(const StreamConfiguration& config) {
    return pimpl_->sendStreamConfiguration(config);
}

bool DriverManager::requestStreamConfigurations() {
    return pimpl_->requestStreamConfigurations();
}

bool DriverManager::startDriver() {
    return pimpl_->startDriver();
}

bool DriverManager::stopDriver() {
    return pimpl_->stopDriver();
}

} // namespace AES67