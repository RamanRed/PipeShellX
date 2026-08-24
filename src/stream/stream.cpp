#include "psx/stream/stream.hpp"

#include <algorithm>
#include <array>
#include <string_view>

namespace psx::stream {

Stream::Stream(std::size_t bufferCapacity, OverflowPolicy policy) : buffer_(bufferCapacity, policy) {}

std::size_t Stream::write(std::span<const char> data) {
    if (state_ != StreamState::Open) {
        return 0; // the write side is closed, failed, or done
    }
    if (buffer_.policy() == OverflowPolicy::Spool) {
        return appendSpool(data);
    }
    return buffer_.append(data);
}

std::size_t Stream::read(std::span<char> out) {
    if (state_ == StreamState::Error) {
        return 0;
    }
    std::size_t total = 0;
    while (total < out.size()) {
        fillFromSpool();
        if (buffer_.empty()) {
            if (buffer_.capacity() == 0 && !spool_.empty()) {
                const std::size_t n = spool_.read(out.subspan(total));
                total += n;
                if (spool_.empty()) {
                    spool_.reset();
                }
                if (n == 0) {
                    break;
                }
                continue;
            }
            break;
        }
        total += buffer_.consume(out.subspan(total));
    }
    maybeCloseAfterDrain();
    return total;
}

std::span<const char> Stream::peek() const {
    fillFromSpool();
    return buffer_.peek();
}

void Stream::drop(std::size_t n) {
    std::array<char, 64 * 1024> discarded{};
    while (n > 0) {
        fillFromSpool();
        const std::size_t fromBuffer = std::min(n, buffer_.size());
        if (fromBuffer > 0) {
            buffer_.drop(fromBuffer);
            n -= fromBuffer;
            continue;
        }
        if (buffer_.capacity() == 0 && !spool_.empty()) {
            const std::size_t fromSpool = spool_.read(std::span<char>(discarded.data(), std::min(n, discarded.size())));
            n -= fromSpool;
            if (spool_.empty()) {
                spool_.reset();
            }
            if (fromSpool == 0) {
                break;
            }
            continue;
        }
        break;
    }
    maybeCloseAfterDrain();
}

std::size_t Stream::appendSpool(std::span<const char> data) {
    if (data.empty()) {
        return 0;
    }
    if (!spool_.empty()) {
        return spool_.append(std::string_view(data.data(), data.size())) ? data.size() : 0;
    }

    const std::size_t accepted = buffer_.append(data);
    if (accepted == data.size()) {
        return accepted;
    }
    const auto overflow = data.subspan(accepted);
    return spool_.append(std::string_view(overflow.data(), overflow.size())) ? data.size() : accepted;
}

void Stream::fillFromSpool() const {
    if (!buffer_.empty() || buffer_.capacity() == 0 || spool_.empty()) {
        return;
    }
    std::array<char, 64 * 1024> chunk{};
    const std::size_t n = spool_.read(std::span<char>(chunk.data(), std::min(chunk.size(), buffer_.capacity())));
    if (n == 0) {
        return;
    }
    (void)buffer_.append(std::span<const char>(chunk.data(), n));
    if (spool_.empty()) {
        spool_.reset();
    }
}

void Stream::maybeCloseAfterDrain() noexcept {
    if (state_ == StreamState::HalfClosedRemote && !readable()) {
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
            spool_.reset();
            break;
        case StreamState::HalfClosedRemote:
            state_ = StreamState::Closed;
            buffer_.clear();
            spool_.reset();
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
    if (buffer_.policy() == OverflowPolicy::Spool) {
        return true;
    }
    // A drop-policy buffer always accepts (it discards to make room), so its
    // producer never blocks; only Block backpressures on a full buffer.
    return dropsOnOverflow(buffer_.policy()) || !buffer_.full();
}

bool Stream::atEnd() const noexcept {
    if (state_ == StreamState::Closed || state_ == StreamState::Error || state_ == StreamState::HalfClosedLocal) {
        return true;
    }
    return state_ == StreamState::HalfClosedRemote && !readable();
}

} // namespace psx::stream
