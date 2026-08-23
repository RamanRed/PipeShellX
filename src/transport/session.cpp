#include "psx/transport/session.hpp"

#include "psx/transport/control_payloads.hpp"

#include <algorithm>

namespace psx::transport {

namespace {
psx::Error protocolError(const char* what) {
    return psx::Error{psx::ErrorClass::Other, 0, what};
}
std::uint8_t channelFlag(Channel channel) {
    return channel == Channel::Stderr ? kFlagStderr : 0;
}
} // namespace

Session::Session(Role role, WriteFn write, SessionHandler& handler, std::uint32_t initialWindow)
    : role_(role), write_(std::move(write)), handler_(handler), initialWindow_(initialWindow) {}

void Session::send(const Frame& frame) {
    std::string wire;
    encodeFrameInto(wire, frame);
    write_(wire);
}

StreamId Session::open(const OpenRequest& request) {
    const StreamId id = nextStreamId_++;
    highestStream_ = std::max(highestStream_, id);
    streams_.try_emplace(id, initialWindow_);
    send(Frame{.type = FrameType::Open, .flags = 0, .streamId = id, .payload = encodeOpen(request)});
    return id;
}

void Session::sendData(StreamId id, std::string_view bytes, bool endStream, Channel channel) {
    auto it = streams_.find(id);
    if (it == streams_.end()) {
        return; // unknown stream: nothing to send
    }
    Stream& stream = it->second;
    if (!bytes.empty()) {
        stream.sendQueue.push_back({channel, std::string(bytes)});
        stream.sendBytes += bytes.size();
    }
    if (endStream) {
        stream.sendEndPending = true;
    }
    flushStream(id, stream);
}

void Session::flushStream(StreamId id, Stream& stream) {
    const bool wasFull = stream.sendBytes >= initialWindow_;
    while (!stream.sendQueue.empty() && stream.sendCredit > 0) {
        Stream::Segment& front = stream.sendQueue.front();
        const auto chunk = static_cast<std::uint32_t>(std::min<std::size_t>(front.bytes.size(), stream.sendCredit));
        // The stream's endStream flag rides the last byte of the last segment.
        const bool drainsQueue = chunk == front.bytes.size() && stream.sendQueue.size() == 1;
        const bool last = drainsQueue && stream.sendEndPending;
        std::uint8_t flags = channelFlag(front.channel);
        if (last) {
            flags |= kFlagEndStream;
        }
        send(Frame{.type = FrameType::Data, .flags = flags, .streamId = id, .payload = front.bytes.substr(0, chunk)});
        stream.sendCredit -= chunk;
        stream.sendBytes -= chunk;
        if (chunk == front.bytes.size()) {
            stream.sendQueue.pop_front();
        } else {
            front.bytes.erase(0, chunk);
        }
        if (last) {
            stream.sendEndPending = false;
        }
    }
    // A pending end that has no (more) buffered bytes rides a zero-length frame.
    if (stream.sendQueue.empty() && stream.sendEndPending) {
        send(Frame{.type = FrameType::Data, .flags = kFlagEndStream, .streamId = id, .payload = {}});
        stream.sendEndPending = false;
    }
    // A deferred EXIT rides out once its DATA has fully drained (terminal).
    if (stream.sendQueue.empty() && !stream.sendEndPending && stream.exitPending) {
        const auto status = stream.exitStatus;
        send(Frame{.type = FrameType::Exit, .flags = kFlagEndStream, .streamId = id, .payload = encodeExit(status)});
        streams_.erase(id); // invalidates `stream`; nothing below may touch it
        return;
    }
    // The buffer drained back below the high-water mark: the producer may resume.
    if (wasFull && stream.sendBytes < initialWindow_ && streamWritable_) {
        streamWritable_(id);
    }
}

bool Session::streamWritable(StreamId id) const {
    auto it = streams_.find(id);
    return it == streams_.end() || it->second.sendBytes < initialWindow_;
}

void Session::consume(StreamId id, std::uint32_t n) {
    auto it = streams_.find(id);
    if (it == streams_.end()) {
        return;
    }
    const std::uint32_t delta = it->second.recvWindow.onConsumed(n);
    if (delta > 0) {
        send(Frame{.type = FrameType::WindowUpdate, .flags = 0, .streamId = id, .payload = encodeWindowUpdate(delta)});
    }
}

void Session::sendExit(StreamId id, const psx::os::ExitStatus& status) {
    auto it = streams_.find(id);
    if (it == streams_.end()) {
        return; // already closed
    }
    Stream& stream = it->second;
    if (!stream.sendQueue.empty() || stream.sendEndPending) {
        // DATA is still flow-controlled in the buffer; EXIT must not overtake it.
        // flushStream() emits it once the buffer drains.
        stream.exitPending = true;
        stream.exitStatus = status;
        return;
    }
    send(Frame{.type = FrameType::Exit, .flags = kFlagEndStream, .streamId = id, .payload = encodeExit(status)});
    streams_.erase(it); // terminal
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
            highestStream_ = std::max(highestStream_, frame.streamId);
            streams_.try_emplace(frame.streamId, initialWindow_);
            handler_.onOpen(frame.streamId, request.value());
            return {};
        }
        case FrameType::Data: {
            auto it = streams_.find(frame.streamId);
            if (it == streams_.end()) {
                // A stream can close (EXIT) while the peer still has DATA in flight
                // for it -- a benign race. Ignore DATA for an id that was opened
                // once; an id we never opened is a real protocol violation.
                if (frame.streamId <= highestStream_) {
                    return {};
                }
                return protocolError("DATA for a stream that was never opened");
            }
            if (!it->second.recvWindow.onData(static_cast<std::uint32_t>(frame.payload.size()))) {
                return protocolError("DATA exceeds the stream's flow-control window");
            }
            const bool endStream = (frame.flags & kFlagEndStream) != 0;
            const Channel channel = (frame.flags & kFlagStderr) != 0 ? Channel::Stderr : Channel::Stdout;
            handler_.onData(frame.streamId, frame.payload, endStream, channel);
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
        case FrameType::WindowUpdate: {
            auto it = streams_.find(frame.streamId);
            if (it == streams_.end()) {
                // A stream can close (EXIT) while the peer still has in-flight
                // WINDOW_UPDATEs for it; credit for a gone stream is a harmless
                // no-op, so ignore it rather than tearing down the connection.
                return {};
            }
            auto delta = decodeWindowUpdate(frame.payload);
            if (!delta.ok()) {
                return delta.error();
            }
            Stream& stream = it->second;
            if (static_cast<std::uint64_t>(stream.sendCredit) + delta.value() > kMaxStreamWindow) {
                return protocolError("WINDOW_UPDATE overflows the stream flow-control window");
            }
            stream.sendCredit += delta.value();
            flushStream(frame.streamId, stream);
            return {};
        }
    }
    return protocolError("unknown frame type");
}

} // namespace psx::transport
