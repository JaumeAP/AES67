#include "FakeRegistry.h"

namespace AES67::Testing {

std::string answer(const char* status, const std::string& body) {
    return std::string("HTTP/1.1 ") + status + "\r\n" +
           "Content-Type: application/json\r\n" +
           "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

}  // namespace AES67::Testing
