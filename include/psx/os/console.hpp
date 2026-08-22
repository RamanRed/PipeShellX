#pragma once

// psx::os::Console — what the interactive client needs from the terminal:
// "is this a terminal?" and a hidden (echo-off) secret prompt.

#include "psx/result.hpp"

#include <cstdint>
#include <string>

namespace psx::os {

enum class StandardStream : std::uint8_t { Input, Output, Error };

bool isInteractive(StandardStream stream) noexcept;

// Writes `prompt`, reads one line from the terminal with echo disabled and
// restores the terminal afterwards. Unsupported when standard input is not a
// terminal; Closed at end of input.
Result<std::string> readSecret(const std::string& prompt);

} // namespace psx::os
