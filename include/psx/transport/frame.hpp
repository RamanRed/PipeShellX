#pragma once

#include <cstdint>
#include <string>

namespace psx::transport {

// Frame types of the psx/1 native backplane (see docs/wire_protocol.md).
// The codec treats the type as an opaque byte; semantics live one layer up, so
// an unknown type decodes cleanly (forward compatibility) rather than erroring.
enum class FrameType : std::uint8_t {
    Open = 1,         // start a stream (payload: encoded job/stage spec)
    Data = 2,         // stream payload bytes
    WindowUpdate = 3, // credit replenishment (payload: big-endian u32 delta)
    Exit = 4,         // stream finished (payload: exit status)
    Ping = 5,         // liveness / lease keep-alive
    Pong = 6,         // ping acknowledgement
    GoAway = 7,       // graceful drain: stop opening new streams
};

// Per-type flag bits. kEndStream marks the last frame of a stream (half-close).
inline constexpr std::uint8_t kFlagEndStream = 0x01;
// On a DATA frame, marks the payload as the stage's stderr; unset means stdout.
inline constexpr std::uint8_t kFlagStderr = 0x02;

// One protocol frame: a typed, flagged, stream-scoped, length-delimited unit.
struct Frame {
    FrameType type{};
    std::uint8_t flags = 0;
    std::uint32_t streamId = 0;
    std::string payload; // may be empty

    bool operator==(const Frame& other) const {
        return type == other.type && flags == other.flags && streamId == other.streamId && payload == other.payload;
    }
};

} // namespace psx::transport
