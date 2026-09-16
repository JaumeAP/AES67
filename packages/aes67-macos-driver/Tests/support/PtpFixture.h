//
// PtpFixture.h
// AES67 macOS Driver tests
// The recorded PTP messages both interop suites replay.
//
// TestPTPT41Interop and TestPTPMasterBoxInterop read the same file, built
// from the Teensy library's own source, and each had written out the same
// struct and the same reader: skip blanks and comments, three
// whitespace-separated fields, the third one hex. Two readers of one file is
// two answers to what that file says.
//
// Whether a missing file is fatal is the suite's own call and stays at the
// call site: one of them cannot run at all without it, the other checks a
// subset and says so.
//
#pragma once

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace AES67::Testing {

/// One line of the fixture: what the library sent, and on which socket.
struct EmittedMessage {
    std::string name;           ///< Sync, Follow_Up, Announce, Delay_Resp
    bool onEventSocket{false};  ///< event is 319, general is 320
    std::vector<uint8_t> bytes;
};

/// Every message in the file, or nothing when it is not there.
inline std::vector<EmittedMessage> readEmittedFixture(const std::string& path) {
    std::vector<EmittedMessage> messages;
    std::ifstream file(path);
    if (!file.is_open()) return messages;

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;

        std::istringstream fields(line);
        std::string name, socketName, hex;
        fields >> name >> socketName >> hex;
        if (hex.empty()) continue;

        EmittedMessage message;
        message.name = name;
        message.onEventSocket = (socketName == "event");
        message.bytes.reserve(hex.size() / 2);
        for (size_t i = 0; i + 1 < hex.size(); i += 2) {
            message.bytes.push_back(
                static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
        }
        messages.push_back(std::move(message));
    }
    return messages;
}

/// Every message of that name, in the order the library sent them.
inline std::vector<EmittedMessage> messagesNamed(const std::vector<EmittedMessage>& messages,
                                                 const std::string& name) {
    std::vector<EmittedMessage> matching;
    for (const EmittedMessage& message : messages) {
        if (message.name == name) matching.push_back(message);
    }
    return matching;
}

/// The first message of that name, or an empty one when there is none.
inline EmittedMessage firstNamed(const std::vector<EmittedMessage>& messages,
                                 const std::string& name) {
    const std::vector<EmittedMessage> matching = messagesNamed(messages, name);
    return matching.empty() ? EmittedMessage{} : matching.front();
}

}  // namespace AES67::Testing
