#pragma once

// psx::os::Handle — owns exactly one kernel object (POSIX fd / Win32 HANDLE),
// closes it exactly once, and is non-inheritable from the moment it exists.
// The raw value is reachable only through psx::os::Backend (src/os/** only).

#include "psx/result.hpp"

#include <cstdint>

namespace psx::os {

using NativeHandle = std::intptr_t; // fits an int fd and a HANDLE
inline constexpr NativeHandle kInvalidHandle = -1;

class Handle {
public:
    Handle() noexcept = default;
    ~Handle();
    Handle(Handle&& other) noexcept;
    Handle& operator=(Handle&& other) noexcept;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    bool valid() const noexcept { return handle_ != kInvalidHandle; }
    explicit operator bool() const noexcept { return valid(); }

    // Idempotent; never throws; the object is invalid afterwards.
    void close() noexcept;

    // A second, independent, non-inheritable handle to the same kernel object.
    Result<Handle> duplicate() const;

    // Readiness-style backends need non-blocking handles; completion-style
    // backends (Windows) ignore this and use overlapped I/O.
    Result<void> setNonBlocking(bool enabled);

    void swap(Handle& other) noexcept;

private:
    friend struct Backend;
    explicit Handle(NativeHandle handle) noexcept : handle_(handle) {}
    NativeHandle handle_ = kInvalidHandle;
};

// Process-wide accounting of handles owned by psx::os::Handle objects.
struct HandleStats {
    std::int64_t open;
    std::int64_t created;
    std::int64_t closed;
};
HandleStats handleStats() noexcept;

} // namespace psx::os
