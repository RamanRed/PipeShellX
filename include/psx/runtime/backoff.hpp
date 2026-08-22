#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace psx::runtime {

// Equal-jitter exponential backoff for a 1-based retry `attempt`:
//   full  = min(cap, base * 2^(attempt-1))      (saturating; cap 0 == no cap)
//   delay = full/2 + rand01 * (full - full/2)
// so delay lands in [full/2, full]. `rand01` (clamped to [0,1]) is injected so
// the function is pure and deterministically testable; the caller supplies a
// uniform draw. Equal jitter keeps a non-zero floor (unlike full jitter), which
// makes it both well-spread and easy to bound in tests.
inline std::chrono::milliseconds
backoffDelay(int attempt, std::chrono::milliseconds base, std::chrono::milliseconds cap, double rand01) {
    if (attempt < 1) {
        attempt = 1;
    }
    const std::uint64_t baseMs = base.count() > 0 ? static_cast<std::uint64_t>(base.count()) : 0;
    const std::uint64_t capMs = cap.count() > 0 ? static_cast<std::uint64_t>(cap.count()) : 0;

    std::uint64_t full = baseMs;
    for (int i = 1; i < attempt && i < 32; ++i) {
        if (capMs != 0 && full >= capMs) {
            break;
        }
        full <<= 1U;
    }
    if (capMs != 0) {
        full = std::min(full, capMs);
    }

    const std::uint64_t half = full / 2;
    rand01 = std::clamp(rand01, 0.0, 1.0);
    const auto jitter = static_cast<std::uint64_t>(rand01 * static_cast<double>(full - half));
    return std::chrono::milliseconds(static_cast<long long>(half + jitter));
}

} // namespace psx::runtime
