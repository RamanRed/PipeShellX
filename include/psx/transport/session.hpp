#pragma once

#include "psx/os/process.hpp" // psx::os::ExitStatus
#include "psx/result.hpp"
#include "psx/transport/frame_codec.hpp"
#include "psx/transport/open_request.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace psx::transport {

using StreamId = std::uint32_t;

// Which side of a connection a Session drives. Only the Controller opens streams
// (to run stages); the Node receives them and reports results.
enum class Role : std::uint8_t { Controller, Node };

// Observer of session events. Callbacks run on the single reactor thread inside
// Session::receive(); the default implementations ignore the event.
class SessionHandler {
public:
    virtual ~SessionHandler() = default;
    virtual void onOpen(StreamId /*id*/, const OpenRequest& /*request*/) {} // Node: a stage was opened
    virtual void onData(StreamId /*id*/, std::string_view /*bytes*/, bool /*endStream*/) {}
    virtual void onExit(StreamId /*id*/, const psx::os::ExitStatus& /*status*/) {} // Controller: the stage exited
    virtual void onGoAway() {}                                                     // peer is draining
    virtual void onPong() {}                                                       // lease keep-alive reply
};

// Multiplexes logical streams over one connection using the psx/1 frame codec.
// Transport-agnostic: outbound bytes go to the `write` callback; inbound bytes
// are handed to receive(). Two Sessions wired write→receive form an in-memory
// loopback for protocol tests. Not thread-safe.
//
// Stream lifecycle in this slice: Controller open() -> OPEN; either side may
// sendData(); the Node's sendExit() is terminal (removes the stream on both
// sides). Credit-window flow control is layered on next.
class Session {
public:
    using WriteFn = std::function<void(std::string_view)>;

    Session(Role role, WriteFn write, SessionHandler& handler);

    Role role() const noexcept { return role_; }
    std::size_t openStreamCount() const noexcept { return streams_.size(); }

    // Controller only: open a stream to run `request`; returns the new stream id.
    StreamId open(const OpenRequest& request);
    // Send stream payload; `endStream` half-closes this side's send direction.
    void sendData(StreamId id, std::string_view bytes, bool endStream);
    // Node only: report the stage's exit; terminal for the stream.
    void sendExit(StreamId id, const psx::os::ExitStatus& status);
    void ping();
    void goAway();

    // Feed received bytes. Dispatches complete frames to the handler. Returns a
    // protocol error (poisoning the underlying decoder) on a malformed frame or a
    // protocol violation (e.g. a frame for an unknown stream); the caller then
    // tears down the connection.
    psx::Result<void> receive(std::string_view bytes);

private:
    struct Stream {};

    void send(const Frame& frame);
    psx::Result<void> dispatch(Frame&& frame);

    Role role_;
    WriteFn write_;
    SessionHandler& handler_;
    FrameDecoder decoder_;
    std::unordered_map<StreamId, Stream> streams_;
    StreamId nextStreamId_ = 1;
    bool goneAway_ = false;
};

} // namespace psx::transport
