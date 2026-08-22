#include "psx/stream/stream.hpp"

namespace psx::stream {

Stream::Stream(std::size_t bufferCapacity, OverflowPolicy policy) : buffer_(bufferCapacity, policy) {}

std::size_t Stream::write(std::span<const char> data) {
    if (state_ != StreamState::Open) {
        return 0; // the write side is closed, failed, or done
    }
    return buffer_.append(data);
}

std::size_t Stream::read(std::span<char> out) {
    if (state_ == StreamState::Error) {
        return 0;
    }
    const std::size_t n = buffer_.consume(out);
    maybeCloseAfterDrain();
    return n;
}

std::span<const char> Stream::peek() const {
    return buffer_.peek();
}

void Stream::drop(std::size_t n) {
    buffer_.drop(n);
    maybeCloseAfterDrain();
}

void Stream::maybeCloseAfterDrain() noexcept {
    if (state_ == StreamState::HalfClosedRemote && buffer_.empty()) {
        state_ = StreamState::Closed;
    }
}

void Stream::closeRemote() noexcept {
    switch (state_) {
        case StreamState::Open:
            // Stay HalfClosedRemote until the buffer is drained (see read());
            // an already-empty stream closes on the sink's next read().
            state_ = StreamState::HalfClosedRemote;
            break;
        case StreamState::HalfClosedLocal:
            state_ = StreamState::Closed;
            break;
        default:
            break; // already terminal or remote-closed
    }
}

void Stream::closeLocal() noexcept {
    switch (state_) {
        case StreamState::Open:
            state_ = StreamState::HalfClosedLocal;
            buffer_.clear(); // the sink will not read the remaining bytes
            break;
        case StreamState::HalfClosedRemote:
            state_ = StreamState::Closed;
            buffer_.clear();
            break;
        default:
            break;
    }
}

void Stream::fail(const Error& error) noexcept {
    if (state_ == StreamState::Closed || state_ == StreamState::Error) {
        return; // a clean close is authoritative
    }
    state_ = StreamState::Error;
    error_ = error;
}

bool Stream::writable() const noexcept {
    if (state_ != StreamState::Open) {
        return false;
    }
    // A drop-policy buffer always accepts (it discards to make room), so its
    // producer never blocks; only Block backpressures on a full buffer.
    return buffer_.policy() != OverflowPolicy::Block || !buffer_.full();
}

bool Stream::atEnd() const noexcept {
    if (state_ == StreamState::Closed || state_ == StreamState::Error || state_ == StreamState::HalfClosedLocal) {
        return true;
    }
    return state_ == StreamState::HalfClosedRemote && buffer_.empty();
}

} // namespace psx::stream
