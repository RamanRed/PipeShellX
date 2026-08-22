#pragma once

// Internal to src/os/posix: errno → psx::Error.

#include "psx/result.hpp"

namespace psx::os::posix {

ErrorClass classify(int err) noexcept;

// Builds an Error for the given operation from an errno value.
inline Error fromErrno(const char* op, int err) noexcept {
    return Error{classify(err), err, op};
}

} // namespace psx::os::posix
