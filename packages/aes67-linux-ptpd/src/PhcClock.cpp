#include "PhcClock.h"

#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <ctime>

namespace AES67::LinuxPtpd {
namespace {

/// The kernel's mapping from a /dev/ptpN descriptor to a clockid_t, as
/// clock_gettime takes it. It is not a header constant: the ABI is defined as
/// this arithmetic (see the kernel's tools/testing/selftests/ptp).
clockid_t clockIdFor(int fd) {
    return static_cast<clockid_t>((~static_cast<unsigned int>(fd) << 3) | 3);
}

/// ethtool's SIOCETHTOOL/ETHTOOL_GET_TS_INFO says which PHC index an
/// interface's timestamps come from. -1 means the driver offers none.
int phcIndexFor(const std::string& interfaceName, std::string& error) {
    const int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        error = std::string("socket() for ethtool: ") + std::strerror(errno);
        return -1;
    }

    struct ethtool_ts_info info {};
    info.cmd = ETHTOOL_GET_TS_INFO;

    struct ifreq request {};
    std::strncpy(request.ifr_name, interfaceName.c_str(), IFNAMSIZ - 1);
    request.ifr_data = reinterpret_cast<char*>(&info);

    const int result = ::ioctl(sock, SIOCETHTOOL, &request);
    const int savedErrno = errno;
    ::close(sock);

    if (result < 0) {
        error = "ETHTOOL_GET_TS_INFO on " + interfaceName + ": " +
                std::strerror(savedErrno);
        return -1;
    }
    if (info.phc_index < 0) {
        error = interfaceName + " reports no PTP hardware clock";
        return -1;
    }
    return info.phc_index;
}

}  // namespace

PhcClock::~PhcClock() {
    if (fd_ >= 0) ::close(fd_);
}

bool PhcClock::open(const std::string& interfaceName, const std::string& device,
                    std::string& error) {
    std::string path = device;
    if (path.empty()) {
        const int index = phcIndexFor(interfaceName, error);
        if (index < 0) return false;
        path = "/dev/ptp" + std::to_string(index);
    }

    const int fd = ::open(path.c_str(), O_RDWR);
    if (fd < 0) {
        error = "open " + path + ": " + std::strerror(errno);
        return false;
    }

    // Reading it once here rather than at the first Sync: a descriptor that
    // opens but does not tick is a configuration problem, and it should be
    // reported at start-up and not three messages into a stream.
    struct timespec now {};
    if (::clock_gettime(clockIdFor(fd), &now) != 0) {
        error = "clock_gettime on " + path + ": " + std::strerror(errno);
        ::close(fd);
        return false;
    }

    fd_ = fd;
    devicePath_ = path;
    return true;
}

uint64_t PhcClock::currentTimeNs() const {
    struct timespec now {};
    const clockid_t clock = fd_ >= 0 ? clockIdFor(fd_) : CLOCK_REALTIME;
    if (::clock_gettime(clock, &now) != 0) return 0;
    return static_cast<uint64_t>(now.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(now.tv_nsec);
}

std::string PhcClock::name() const {
    if (fd_ >= 0) return "PHC " + devicePath_;
    return "CLOCK_REALTIME (no hardware clock)";
}

void PhcClock::setAnnouncedQuality(uint8_t clockClass, PTPClockAccuracy accuracy) {
    clockClass_ = clockClass;
    accuracy_ = accuracy;
}

}  // namespace AES67::LinuxPtpd
