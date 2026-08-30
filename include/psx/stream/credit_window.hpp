#pragma once

// psx::stream::CreditWindow — one direction of HTTP/2-style flow control for
// the native backplane (L2, consumed by the native transport). It is the
// receiver's view: the peer may send up to `sendable()` bytes; each arriving
// DATA frame calls onData() (rejected if it would exceed the window); as the
// sink drains buffered bytes, onConsumed() accumulates and, once at least
// `updateThreshold` bytes have been freed, returns the increment to advertise
// in a WINDOW_UPDATE (default threshold: half the window, so credit is
// replenished before the sender stalls).

#include <cstdint>

namespace psx::stream {

class CreditWindow {
public:
    // updateThreshold defaults to window/2 when 0.
    explicit CreditWindow(std::uint32_t window, std::uint32_t updateThreshold = 0);

    std::uint32_t window() const noexcept { return window_; }
    std::uint32_t updateThreshold() const noexcept { return threshold_; }

    // Bytes granted-but-not-yet-consumed, and what the peer may still send.
    std::uint32_t outstanding() const noexcept { return outstanding_; }
    std::uint32_t sendable() const noexcept { return window_ - outstanding_; }

    // A DATA frame of n bytes arrived. Returns false (no state change) if it
    // would push `outstanding` past the window — a flow-control violation.
    bool onData(std::uint32_t n) noexcept;

    // The sink consumed n buffered bytes (clamped to outstanding). Returns the
    // WINDOW_UPDATE increment to advertise — 0 until the accumulated freed
    // bytes reach the threshold, unless all outstanding data has now been
    // consumed, in which case the remainder is advertised at once — and resets
    // the accumulator when it fires.
    std::uint32_t onConsumed(std::uint32_t n) noexcept;

private:
    std::uint32_t window_;
    std::uint32_t threshold_;
    std::uint32_t outstanding_ = 0;   // delivered but not yet consumed
    std::uint32_t pendingUpdate_ = 0; // consumed but not yet advertised
};

} // namespace psx::stream
