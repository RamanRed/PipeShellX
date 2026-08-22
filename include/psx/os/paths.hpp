#pragma once

// Well-known per-user locations (XDG on POSIX, %APPDATA% family on Windows
// in Phase 3).

#include <string>

namespace psx::os {

// $HOME, else the account database, else "" (never throws).
std::string homeDirectory();

// Where `application` keeps state such as logs: $XDG_STATE_HOME/<app>, else
// <home>/.local/state/<app>, else <app> relative to the working directory.
std::string stateDirectory(const std::string& application);

} // namespace psx::os
