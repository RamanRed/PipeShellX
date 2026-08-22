#pragma once

// Byte I/O on stream handles (pipes, sockets, files).

#include "psx/os/handle.hpp"
#include "psx/result.hpp"

#include <cstddef>
#include <span>

namespace psx::os {

// Reads up to buffer.size() bytes. 0 means end of stream. Retries EINTR;
// reports WouldBlock on a non-blocking handle with nothing to read.
Result<std::size_t> read(const Handle& handle, std::span<char> buffer);

// Writes up to data.size() bytes (may be partial). Reports BrokenPipe when the
// other end is gone and WouldBlock when a non-blocking pipe is full.
Result<std::size_t> write(const Handle& handle, std::span<const char> data);

// Makes a write to a closed pipe an ordinary BrokenPipe error instead of a
// process-killing SIGPIPE. Idempotent; a no-op on platforms without it.
Result<void> ignoreBrokenPipeSignal();

} // namespace psx::os
