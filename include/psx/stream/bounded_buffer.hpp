#pragma once

// psx::stream::BoundedBuffer — a capacity-bounded FIFO byte buffer (L2). The
// controller keeps one per active stream so a chatty producer can never grow
// memory without bound. What happens on overflow is the OverflowPolicy:
//
//   Block       accept only what fits; the caller stops reading its source,
//               the kernel pipe fills, and the producer's write(2) blocks.
//               Lossless — the default.
//   DropNewest  accept what fits, discard the rest of the incoming bytes;
//               keeps the oldest data (drops counted).
//   DropOldest  evict the oldest buffered bytes to make room for the newest;
//               keeps the freshest data — log tailing (drops counted).
//
// (Spool — overflow to a temp file — is a later addition.)

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace psx::stream {

// Block: backpressure (retry later). DropNewest/DropOldest: bound memory by
// discarding. Spool: bound memory by spilling overflow to a temp file (no loss)
// — only the output-*capture* honours it; an in-memory ring treats it as Block.
enum class OverflowPolicy : std::uint8_t { Block, DropNewest, DropOldest, Spool };

// True for the policies that discard bytes on overflow (Block and Spool do not).
inline bool dropsOnOverflow(OverflowPolicy policy) noexcept {
    return policy == OverflowPolicy::DropNewest || policy == OverflowPolicy::DropOldest;
}

class BoundedBuffer {
public:
    explicit BoundedBuffer(std::size_t capacity, OverflowPolicy policy = OverflowPolicy::Block);

    // Appends bytes according to the policy. Returns how many bytes of `data`
    // the caller no longer needs to retain: for Block that is only what fit
    // (retry the rest later); for the drop policies it is always data.size()
    // (whatever did not fit was dropped and counted).
    std::size_t append(std::span<const char> data);

    // Copies up to out.size() bytes from the front; returns bytes copied.
    std::size_t consume(std::span<char> out);

    // The contiguous run of buffered bytes at the front (up to the ring wrap);
    // may be shorter than size(). Empty when the buffer is empty.
    std::span<const char> peek() const;
    // Discards up to n bytes from the front.
    void drop(std::size_t n);

    // Empties the buffer; the dropped-byte tally is preserved.
    void clear() noexcept;

    std::size_t size() const noexcept { return size_; }
    std::size_t capacity() const noexcept { return capacity_; }
    std::size_t available() const noexcept { return capacity_ - size_; }
    bool empty() const noexcept { return size_ == 0; }
    bool full() const noexcept { return size_ == capacity_; }
    std::uint64_t droppedBytes() const noexcept { return dropped_; }
    OverflowPolicy policy() const noexcept { return policy_; }

private:
    std::size_t appendUpTo(std::span<const char> data); // ring copy, no policy
    void discardFront(std::size_t n) noexcept;

    std::vector<char> storage_;
    std::size_t capacity_;
    std::size_t head_ = 0; // index of the oldest byte
    std::size_t size_ = 0; // bytes currently buffered
    std::uint64_t dropped_ = 0;
    OverflowPolicy policy_;
};

} // namespace psx::stream
