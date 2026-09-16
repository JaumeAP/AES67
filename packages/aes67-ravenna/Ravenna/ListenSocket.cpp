#include "Ravenna/ListenSocket.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace AES67::Ravenna {

bool openListenSocket(uint16_t& port, int& fd, std::string& error, const char* bindHint) {
    fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        error = std::string("socket(): ") + std::strerror(errno);
        return false;
    }

    int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    struct sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) < 0) {
        error = "bind " + std::to_string(port) + ": " + std::strerror(errno);
        if (errno == EACCES && bindHint != nullptr) error += bindHint;
        ::close(fd);
        fd = -1;
        return false;
    }

    if (::listen(fd, 8) < 0) {
        error = std::string("listen(): ") + std::strerror(errno);
        ::close(fd);
        fd = -1;
        return false;
    }

    if (port == 0) {
        // Port zero means "any", and the caller has to be told which, or the
        // SRV record advertises a port nothing is listening on.
        socklen_t length = sizeof(address);
        if (::getsockname(fd, reinterpret_cast<struct sockaddr*>(&address), &length) == 0) {
            port = ntohs(address.sin_port);
        }
    }
    return true;
}

}  // namespace AES67::Ravenna
