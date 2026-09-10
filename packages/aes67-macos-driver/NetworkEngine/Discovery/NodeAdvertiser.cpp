//
// NodeAdvertiser.cpp
// AES67 macOS Driver
//

#include <algorithm>
#include "NetworkEngine/Discovery/NodeAdvertiser.h"

#include <chrono>

namespace AES67 {

Ravenna::SessionAdvertisement nodeAdvertisement(const std::string& label,
                                                const std::string& hostName,
                                                uint32_t addressV4, uint16_t apiPort) {
    Ravenna::SessionAdvertisement node;
    // An instance name is ONE DNS label: a dot inside it is a separator on
    // the wire, so a label built from gethostname() ("... on macmini.local")
    // would make a PTR of two labels and mDNSResponder drops the record.
    // Same rule, and the same replacement, as aes67-ravenna's oneLabel().
    node.instanceName = label;
    std::replace(node.instanceName.begin(), node.instanceName.end(), '.', ' ');
    node.hostName = hostName;
    node.port = apiPort;
    node.addressV4 = addressV4;
    node.serviceType = Ravenna::kNmosNodeService;
    node.subtype.clear();
    // IS-04 sec 3: what a controller reads before it opens a connection.
    node.txtEntries = {"api_ver=v1.3", "api_proto=http", "api_auth=false", "ver_slf=0"};
    return node;
}

NodeAdvertiser::NodeAdvertiser() = default;

NodeAdvertiser::~NodeAdvertiser() { stop(); }

bool NodeAdvertiser::start(const std::string& interfaceName,
                           const Ravenna::SessionAdvertisement& advertisement,
                           std::string& error) {
    stop();
    responder_ = std::make_unique<Ravenna::MdnsResponder>(noSessions_);
    responder_->alsoAdvertise(advertisement);
    // The RTSP port the responder is told about is never advertised: the
    // catalogue is empty, so no session record carries it.
    if (!responder_->start(interfaceName, advertisement.hostName, advertisement.addressV4,
                           advertisement.port, error)) {
        responder_.reset();
        return false;
    }
    running_.store(true);
    thread_ = std::thread([this] {
        int announcements = 0;
        auto nextAnnouncement = std::chrono::steady_clock::now();
        while (running_.load()) {
            if (announcements < 3 && std::chrono::steady_clock::now() >= nextAnnouncement) {
                responder_->announce();
                ++announcements;
                nextAnnouncement += std::chrono::seconds(1);
            }
            responder_->service();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
    return true;
}

void NodeAdvertiser::stop() {
    if (!running_.exchange(false)) {
        responder_.reset();
        return;
    }
    if (thread_.joinable()) thread_.join();
    if (responder_) responder_->goodbye();
    responder_.reset();
}

}  // namespace AES67
