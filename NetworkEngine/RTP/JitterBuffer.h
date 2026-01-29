#ifndef JITTER_BUFFER_H
#define JITTER_BUFFER_H

#include "LockFreeCircularJitterBuffer.h"  // Use the new lock-free implementation
#include <atomic>
#include <memory>
#include <cstdint>

namespace AES67 {

// Redirect to the new lock-free implementation
using JitterBuffer = LockFreeCircularJitterBuffer;

} // namespace AES67

#endif // JITTER_BUFFER_H