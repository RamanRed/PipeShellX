#pragma once

#include "psx/result.hpp"
#include "psx/transport/frame.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <string>

namespace psx::transport {

// Wire envelope (TLV, network byte order): a 10-byte fixed header followed by
// `length` payload bytes.
//   type      : u8
//   flags     : u8
//   streamId  : u32 big-endian
//   length    : u32 big-endian
inline constexpr std::size_t kFrameHeaderSize = 10;

// A payload longer than this is rejected as a protocol violation, bounding the
// decoder's buffer against a malicious/garbage length. 16 MiB.
inline constexpr std::uint32_t kDefaultMaxFramePayload = 16U * 1024 * 1024;

// Serialises one frame to its wire bytes (header + payload).
std::string encodeFrame(const Frame& frame);
void encodeFrameInto(std::string& out, const Frame& frame);

// Streaming decoder: fed arbitrary byte chunks, it emits every complete frame in
// order via the sink. A frame split across chunks is buffered until complete;
// several frames in one chunk are all emitted. A payload length exceeding
// maxPayload is a protocol violation: push() returns an error and the decoder is
// poisoned (all further push() calls error) so a bad peer cannot desync the
// stream. Not thread-safe — used from the single reactor thread.
class FrameDecoder {
public:
    using FrameSink = std::function<void(Frame&&)>;

    explicit FrameDecoder(std::uint32_t maxPayload = kDefaultMaxFramePayload) : maxPayload_(maxPayload) {}

    psx::Result<void> push(std::span<const char> data, const FrameSink& sink);

    // True when bytes of an incomplete frame are buffered (not on a frame boundary).
    bool hasPartialFrame() const noexcept { return !buffer_.empty(); }

private:
    std::string buffer_;
    std::uint32_t maxPayload_;
    bool failed_ = false;
};

} // namespace psx::transport
