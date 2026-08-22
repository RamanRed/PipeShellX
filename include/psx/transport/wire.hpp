#pragma once

#include <cstdint>
#include <string>

namespace psx::transport {

// Big-endian (network order) 32-bit helpers shared by the frame envelope and the
// per-type payload codecs.
inline void writeU32BE(std::string& out, std::uint32_t value) {
    out.push_back(static_cast<char>((value >> 24) & 0xFF));
    out.push_back(static_cast<char>((value >> 16) & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>(value & 0xFF));
}

// Reads a big-endian u32 from `p`; the caller guarantees at least 4 readable bytes.
inline std::uint32_t readU32BE(const char* p) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(p[0])) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(p[1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(p[2])) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(p[3]));
}

} // namespace psx::transport
