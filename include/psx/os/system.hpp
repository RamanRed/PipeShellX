#pragma once

// Process-wide facts and knobs that every layer needs: our own process id,
// the open-handle limit, and an executable check for trusted-path lookups.

#include "psx/os/process.hpp"
#include "psx/result.hpp"

#include <cstdint>
#include <string>

namespace psx::os {

ProcessId currentProcessId() noexcept;

struct HandleLimit {
    std::uint64_t soft;
    std::uint64_t hard;
};

// Raises the soft open-handle limit as far as the hard limit (and the
// platform ceiling, e.g. OPEN_MAX on Darwin) allow. Never lowers it.
Result<HandleLimit> raiseHandleLimit();

// True for a regular file this process may execute (access(X_OK) semantics).
bool isExecutableFile(const std::string& path) noexcept;

} // namespace psx::os
