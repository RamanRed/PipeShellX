#pragma once

// psx::stream::Stream — a unidirectional byte channel with the pipe state
// machine (L2). Bytes written by the producer are held in a BoundedBuffer and
// drained by the sink; EOF and half-close propagate exactly as they do across
// a local pipe.
//
//   Open ──closeRemote()──▶ HalfClosedRemote ──buffer drained──▶ Closed
//    │                                                              ▲
//    └──closeLocal()──▶ HalfClosedLocal ──closeRemote()────────────┘
//   any state ──fail()──▶ Error   (a clean Closed is never downgraded)
//
// "remote" is the writer (the far end that produces bytes); "local" is the
// sink. A drop-policy buffer never blocks the producer; a Block buffer signals
// backpressure through writable(), which the reactor turns into read-interest
// deregistration.

#include "psx/result.hpp"
#include "psx/stream/bounded_buffer.hpp"

#include <cstddef>
#include <optional>
#include <span>

namespace psx::stream {

enum class StreamState : std::uint8_t { Open, HalfClosedRemote, HalfClosedLocal, Closed, Error };

class Stream {
public:
    explicit Stream(std::size_t bufferCapacity, OverflowPolicy policy = OverflowPolicy::Block);

    StreamState state() const noexcept { return state_; }

    // Producer → buffer. Accepted only while the remote (write) side is open;
    // returns the BoundedBuffer accept count (0 once the stream is not Open).
    std::size_t write(std::span<const char> data);

    // Sink ← buffer. Draining the last byte after a remote EOF closes the stream.
    std::size_t read(std::span<char> out);
    std::span<const char> peek() const;
    void drop(std::size_t n);

    void closeRemote() noexcept; // the writer sent EOF
    void closeLocal() noexcept;  // the sink is done (discards any buffered bytes)
    void fail(const Error& error) noexcept;

    bool readable() const noexcept { return buffer_.size() > 0; }
    // True while the producer may make progress: Open, and — for a Block
    // buffer only — not full. Drop-policy streams are always writable.
    bool writable() const noexcept;
    bool full() const noexcept { return buffer_.full(); }
    // No more bytes will ever be read: remote closed and the buffer is empty,
    // or the stream failed/closed.
    bool atEnd() const noexcept;
    bool finished() const noexcept { return state_ == StreamState::Closed || state_ == StreamState::Error; }

    std::size_t buffered() const noexcept { return buffer_.size(); }
    std::uint64_t droppedBytes() const noexcept { return buffer_.droppedBytes(); }
    const std::optional<Error>& error() const noexcept { return error_; }

private:
    void maybeCloseAfterDrain() noexcept;

    BoundedBuffer buffer_;
    StreamState state_ = StreamState::Open;
    std::optional<Error> error_;
};

} // namespace psx::stream
