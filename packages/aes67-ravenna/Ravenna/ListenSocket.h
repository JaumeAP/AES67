//
// ListenSocket.h
// AES67 RAVENNA
// Opening a TCP port to listen on, which both servers in this package do the
// same way.
//
// HttpServer::start and RtspServer::start carried the same twenty lines --
// socket, SO_REUSEADDR, bind, listen, and a getsockname when the caller asked
// for port 0 because the mDNS record has to advertise the port that was
// actually taken. The only thing that differed was one sentence of advice in
// the bind error, which is why the hint is a parameter.
//
#pragma once

#include <cstdint>
#include <string>

namespace AES67::Ravenna {

/// Opens a listening socket on `port`, or on one the kernel picks when `port`
/// is 0. On success `fd` is the listening socket and `port` is the one it
/// actually got; on failure nothing is left open and `error` says why.
///
/// `bindHint` is appended to a bind failure the caller can explain better
/// than errno can -- a privileged port, say.
bool openListenSocket(uint16_t& port, int& fd, std::string& error,
                      const char* bindHint = nullptr);

}  // namespace AES67::Ravenna
