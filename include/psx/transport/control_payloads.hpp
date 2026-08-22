#pragma once

#include "psx/os/process.hpp" // psx::os::ExitStatus
#include "psx/result.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace psx::transport {

// EXIT frame payload — the remote stage's outcome. Wire (5 bytes):
//   u8  kind  (0 Exited, 1 Signaled, 2 Terminated)
//   i32 code  (big-endian two's-complement)
std::string encodeExit(const psx::os::ExitStatus& status);
psx::Result<psx::os::ExitStatus> decodeExit(std::string_view payload);

// WINDOW_UPDATE frame payload — a credit replenishment. Wire (4 bytes):
//   u32 delta (big-endian). A zero delta is a protocol violation (HTTP/2 §6.9).
std::string encodeWindowUpdate(std::uint32_t delta);
psx::Result<std::uint32_t> decodeWindowUpdate(std::string_view payload);

} // namespace psx::transport
