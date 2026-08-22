#include "psx/stream/credit_window.hpp"

#include <algorithm>

namespace psx::stream {

CreditWindow::CreditWindow(std::uint32_t window, std::uint32_t updateThreshold)
    : window_(window), threshold_(updateThreshold != 0 ? updateThreshold : window / 2) {}

bool CreditWindow::onData(std::uint32_t n) noexcept {
    if (n > sendable()) {
        return false;
    }
    outstanding_ += n;
    return true;
}

std::uint32_t CreditWindow::onConsumed(std::uint32_t n) noexcept {
    n = std::min(n, outstanding_);
    outstanding_ -= n;
    pendingUpdate_ += n;
    // Advertise once the accumulated freed credit reaches the threshold, or as
    // soon as everything delivered has been consumed (no reason to sit on the
    // remaining credit while the sender may be waiting for it).
    if (pendingUpdate_ > 0 && (pendingUpdate_ >= threshold_ || outstanding_ == 0)) {
        const std::uint32_t increment = pendingUpdate_;
        pendingUpdate_ = 0;
        return increment;
    }
    return 0;
}

} // namespace psx::stream
