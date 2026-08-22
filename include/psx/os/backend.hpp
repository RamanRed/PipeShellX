#pragma once

// The only door to a Handle's native value. May be included from src/os/**
// (and the os test-suite) exclusively — enforced by the CI layering check.

#include "psx/os/handle.hpp"

namespace psx::os {

struct Backend {
    static NativeHandle native(const Handle& handle) noexcept { return handle.handle_; }

    // Takes ownership of an already non-inheritable native handle.
    static Handle adopt(NativeHandle handle) noexcept;

    // Gives up ownership without closing; the caller now owns the native handle.
    static NativeHandle release(Handle& handle) noexcept;
};

} // namespace psx::os
