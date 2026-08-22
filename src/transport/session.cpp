#include "psx/transport/session.hpp"

#include "psx/transport/control_payloads.hpp"

namespace psx::transport {

namespace {
psx::Error protocolError(const char* what) {
    return psx::Error{psx::ErrorClass::Other, 0, what};
}
} // namespace

Session::Session(Role role, WriteFn write, SessionHandler& handler)
    : role_(role), write_(std::move(write)), handler_(handler) {}

void Session::send(const Frame& frame) {
    std::string wire;
    encodeFrameInto(wire, frame);
    write_(wire);
}

StreamId Session::open(const OpenRequest& request) {
    const StreamId id = nextStreamId_++;
    streams_.emplace(id, Stream{});
    send(Frame{.type = FrameType::Open, .flags = 0, .streamId = id, .payload = encodeOpen(request)});
    return id;
}

void Session::sendData(StreamId id, std::string_view bytes, bool endStream) {
    send(Frame{.type = FrameType::Data,
               .flags = static_cast<std::uint8_t>(endStream ? kFlagEndStream : 0),
               .streamId = id,
               .payload = std::string(bytes)});
}

void Session::sendExit(StreamId id, const psx::os::ExitStatus& status) {
    send(Frame{.type = FrameType::Exit, .flags = kFlagEndStream, .streamId = id, .payload = encodeExit(status)});
    streams_.erase(id); // terminal
}

void Session::ping() {
    send(Frame{.type = FrameType::Ping, .flags = 0, .streamId = 0, .payload = {}});
}

void Session::goAway() {
    send(Frame{.type = FrameType::GoAway, .flags = 0, .streamId = 0, .payload = {}});
}

psx::Result<void> Session::receive(std::string_view bytes) {
    // Collect frames first, then dispatch, so a handler callback can safely call
    // back into this Session (e.g. reply on a stream) without reentering the
    // decoder's buffer.
    psx::Result<void> dispatchResult{};
    auto sink = [&](Frame&& frame) {
        if (dispatchResult.ok()) {
            dispatchResult = dispatch(std::move(frame));
        }
    };
    if (auto decoded = decoder_.push(std::span<const char>(bytes.data(), bytes.size()), sink); !decoded.ok()) {
        return decoded.error();
    }
    return dispatchResult;
}

psx::Result<void> Session::dispatch(Frame&& frame) {
    switch (frame.type) {
        case FrameType::Open: {
            if (role_ != Role::Node) {
                return protocolError("OPEN received by a controller");
            }
            if (streams_.count(frame.streamId) != 0) {
                return protocolError("OPEN for an already-open stream");
            }
            auto request = decodeOpen(frame.payload);
            if (!request.ok()) {
                return request.error();
            }
            streams_.emplace(frame.streamId, Stream{});
            handler_.onOpen(frame.streamId, request.value());
            return {};
        }
        case FrameType::Data: {
            if (streams_.count(frame.streamId) == 0) {
                return protocolError("DATA for an unknown stream");
            }
            const bool endStream = (frame.flags & kFlagEndStream) != 0;
            handler_.onData(frame.streamId, frame.payload, endStream);
            return {};
        }
        case FrameType::Exit: {
            if (role_ != Role::Controller) {
                return protocolError("EXIT received by a node");
            }
            if (streams_.count(frame.streamId) == 0) {
                return protocolError("EXIT for an unknown stream");
            }
            auto status = decodeExit(frame.payload);
            if (!status.ok()) {
                return status.error();
            }
            streams_.erase(frame.streamId); // terminal
            handler_.onExit(frame.streamId, status.value());
            return {};
        }
        case FrameType::Ping:
            send(Frame{.type = FrameType::Pong, .flags = 0, .streamId = 0, .payload = {}}); // auto-reply
            return {};
        case FrameType::Pong:
            handler_.onPong();
            return {};
        case FrameType::GoAway:
            goneAway_ = true;
            handler_.onGoAway();
            return {};
        case FrameType::WindowUpdate:
            // Flow control is layered on in the next slice; ignore for now.
            return {};
    }
    return protocolError("unknown frame type");
}

} // namespace psx::transport
