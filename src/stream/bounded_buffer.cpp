#include "psx/stream/bounded_buffer.hpp"

#include <algorithm>
#include <cstring>

namespace psx::stream {

BoundedBuffer::BoundedBuffer(std::size_t capacity, OverflowPolicy policy)
    : storage_(capacity), capacity_(capacity), policy_(policy) {}

std::size_t BoundedBuffer::appendUpTo(std::span<const char> data) {
    const std::size_t n = std::min(data.size(), available());
    if (n == 0) {
        return 0;
    }
    const std::size_t tail = (head_ + size_) % capacity_;
    const std::size_t firstRun = std::min(n, capacity_ - tail);
    std::memcpy(storage_.data() + tail, data.data(), firstRun);
    if (n > firstRun) {
        std::memcpy(storage_.data(), data.data() + firstRun, n - firstRun);
    }
    size_ += n;
    return n;
}

void BoundedBuffer::discardFront(std::size_t n) noexcept {
    n = std::min(n, size_);
    head_ = (head_ + n) % (capacity_ == 0 ? 1 : capacity_);
    size_ -= n;
}

std::size_t BoundedBuffer::append(std::span<const char> data) {
    if (capacity_ == 0 || data.empty()) {
        if (policy_ != OverflowPolicy::Block) {
            dropped_ += data.size();
        }
        return policy_ == OverflowPolicy::Block ? 0 : data.size();
    }

    switch (policy_) {
        case OverflowPolicy::Block:
            return appendUpTo(data);

        case OverflowPolicy::DropNewest: {
            const std::size_t accepted = appendUpTo(data);
            dropped_ += data.size() - accepted;
            return data.size();
        }

        case OverflowPolicy::DropOldest: {
            if (data.size() >= capacity_) {
                // Only the last `capacity_` bytes can survive; the rest, plus
                // everything already buffered, is dropped.
                dropped_ += size_ + (data.size() - capacity_);
                clear();
                appendUpTo(data.subspan(data.size() - capacity_));
                return data.size();
            }
            if (data.size() > available()) {
                const std::size_t evict = data.size() - available();
                dropped_ += evict;
                discardFront(evict);
            }
            appendUpTo(data);
            return data.size();
        }
    }
    return 0;
}

std::size_t BoundedBuffer::consume(std::span<char> out) {
    const std::size_t n = std::min(out.size(), size_);
    std::size_t copied = 0;
    while (copied < n) {
        const std::size_t run = std::min(n - copied, capacity_ - head_);
        std::memcpy(out.data() + copied, storage_.data() + head_, run);
        head_ = (head_ + run) % capacity_;
        copied += run;
    }
    size_ -= n;
    return n;
}

std::span<const char> BoundedBuffer::peek() const {
    if (size_ == 0) {
        return {};
    }
    const std::size_t run = std::min(size_, capacity_ - head_);
    return std::span<const char>(storage_.data() + head_, run);
}

void BoundedBuffer::drop(std::size_t n) {
    discardFront(n);
}

void BoundedBuffer::clear() noexcept {
    head_ = 0;
    size_ = 0;
}

} // namespace psx::stream
