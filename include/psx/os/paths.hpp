#pragma once

// Well-known per-user locations (XDG on POSIX; a future Windows backend will
// provide the corresponding Windows directories).

#include "psx/result.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace psx::os {

namespace detail {

// Internal implementation seam for testing the POSIX privilege boundary
// without changing process credentials in a unit test.
inline constexpr bool credentialsPermitEnvironment(std::uintmax_t realUser,
                                                   std::uintmax_t effectiveUser,
                                                   std::uintmax_t realGroup,
                                                   std::uintmax_t effectiveGroup) noexcept {
    return realUser == effectiveUser && realGroup == effectiveGroup;
}

} // namespace detail

// True when environment-derived paths have the same user/group authority as
// the process. False for set-user-ID or set-group-ID execution.
bool environmentPathsAreTrusted() noexcept;

// $HOME when environment paths are trusted, else the effective user's account
// database entry, else "" (never throws).
std::string homeDirectory();

// Where `application` keeps state such as logs: trusted
// $XDG_STATE_HOME/<app>, else <home>/.local/state/<app>, else <app> relative to
// the working directory.
std::string stateDirectory(const std::string& application);

// Replaces `path` atomically with `contents` using a same-directory temporary
// file. New files are private to the user; an existing file keeps its mode.
// The temporary is removed on every failure.
Result<void> atomicRewriteFile(const std::string& path, std::string_view contents);

// Atomically replaces `path` with a regular file containing `contents`. The
// replacement is private (0600) before any secret bytes are written, regardless
// of the caller's umask or the mode/type of an existing destination.
Result<void> atomicWritePrivateFile(const std::string& path, std::string_view contents);

} // namespace psx::os
