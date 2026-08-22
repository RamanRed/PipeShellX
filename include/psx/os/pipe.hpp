#pragma once

// psx::os::Pipe — an anonymous unidirectional byte channel: {reader, writer}.
// Both ends are non-inheritable at creation and blocking by default.

#include "psx/os/handle.hpp"
#include "psx/result.hpp"

namespace psx::os {

struct Pipe {
    Handle reader;
    Handle writer;

    static Result<Pipe> create();
};

} // namespace psx::os
