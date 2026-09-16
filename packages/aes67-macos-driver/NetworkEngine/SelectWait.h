//
// SelectWait.h
// AES67 macOS Driver
//
// One answer to "what did select() just tell me", shared by every receive
// loop in the network engine.
//
// Written after an audit (2026-09-04) found six loops built on select() and
// three different reactions to a negative return among them: RTSPServer and
// MDNSBrowser left the loop on EINTR — a signal, which a process gets for
// ordinary reasons — and never came back, while running_ stayed true so the
// object still reported itself alive; RTCPMonitor, PTPPeerObserver and
// RTPReceiver did the opposite and continued, which turns a permanently bad
// descriptor into a loop spinning at full speed with nothing to wait on.
//
// The distinction the callers actually need is three-way, so that is what
// this returns: retry, keep waiting, or give up.
//

// The second thing every one of those loops shares is FD_SET, and none of them
// bounded the descriptor either. On macOS fd_set is a fixed 1024-bit bitmap and
// in every loop it is a stack local, so FD_SET(1500, &rd) writes past the end of
// a 128-byte object and select() with nfds > FD_SETSIZE is undefined on top of
// that. The driver runs inside coreaudiod, whose descriptor table is shared with
// every other HAL plug-in and where nothing caps what number a socket gets, so
// the guard belongs here rather than being restated at nine call sites.
//

#pragma once

#include <sys/select.h>
#include <sys/time.h>

#include <cerrno>

namespace AES67 {

/// FD_SET that refuses a descriptor fd_set cannot hold. Returns false when the
/// descriptor is out of range, so the caller can drop it instead of corrupting
/// its own stack frame; `maxFd` is only advanced for a descriptor that was set.
inline bool addReadable(int fd, fd_set* readfds, int& maxFd) noexcept {
    if (fd < 0 || fd >= FD_SETSIZE) return false;
    FD_SET(fd, readfds);
    if (fd > maxFd) maxFd = fd;
    return true;
}

/// What one select() call means to a receive loop.
enum class SelectOutcome {
    Ready,        ///< at least one descriptor is readable
    Timeout,      ///< nothing happened before the deadline; re-check running_
    Interrupted,  ///< EINTR: a signal, not a failure — call again
    Failed        ///< the descriptor set is unusable; stop, do not spin
};

/// select() for readability on `maxFd`-and-below, waiting at most
/// `timeoutMs`. `readfds` is filled in by the caller and updated in place.
inline SelectOutcome waitReadable(int maxFd, fd_set* readfds, int timeoutMs) noexcept {
    // Backstop for a caller that filled the set without addReadable(): nfds
    // above FD_SETSIZE is undefined, and "undefined" here means reading past
    // the caller's own stack object.
    if (maxFd < 0 || maxFd >= FD_SETSIZE) return SelectOutcome::Failed;
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    const int ready = ::select(maxFd + 1, readfds, nullptr, nullptr, &tv);
    if (ready > 0) return SelectOutcome::Ready;
    if (ready == 0) return SelectOutcome::Timeout;
    return errno == EINTR ? SelectOutcome::Interrupted : SelectOutcome::Failed;
}

} // namespace AES67
