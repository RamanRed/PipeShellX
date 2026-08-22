#include "psx/transport/frame_codec.hpp"

#include "psx/transport/wire.hpp"

namespace psx::transport {

void encodeFrameInto(std::string& out, const Frame& frame) {
    out.reserve(out.size() + kFrameHeaderSize + frame.payload.size());
    out.push_back(static_cast<char>(static_cast<std::uint8_t>(frame.type)));
    out.push_back(static_cast<char>(frame.flags));
    writeU32BE(out, frame.streamId);
    writeU32BE(out, static_cast<std::uint32_t>(frame.payload.size()));
    out.append(frame.payload);
}

std::string encodeFrame(const Frame& frame) {
    std::string out;
    encodeFrameInto(out, frame);
    return out;
}

psx::Result<void> FrameDecoder::push(std::span<const char> data, const FrameSink& sink) {
    if (failed_) {
        return psx::Error{psx::ErrorClass::Other, 0, "frame decoder poisoned by an earlier protocol error"};
    }
    buffer_.append(data.data(), data.size());

    std::size_t pos = 0;
    while (buffer_.size() - pos >= kFrameHeaderSize) {
        const char* header = buffer_.data() + pos;
        const auto length = readU32BE(header + 6);
        if (length > maxPayload_) {
            failed_ = true;
            return psx::Error{psx::ErrorClass::Other, 0, "frame payload length exceeds the maximum"};
        }
        if (buffer_.size() - pos < kFrameHeaderSize + length) {
            break; // header seen, payload still incomplete
        }
        Frame frame;
        frame.type = static_cast<FrameType>(static_cast<std::uint8_t>(header[0]));
        frame.flags = static_cast<std::uint8_t>(header[1]);
        frame.streamId = readU32BE(header + 2);
        frame.payload.assign(header + kFrameHeaderSize, length);
        pos += kFrameHeaderSize + length;
        sink(std::move(frame));
    }
    // Compact once per push (O(n)); keep the trailing partial frame.
    if (pos > 0) {
        buffer_.erase(0, pos);
    }
    return {};
}

} // namespace psx::transport
