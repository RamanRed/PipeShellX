#include "psx/os/paths.hpp"

#include <cstdlib>
#include <filesystem>
#include <pwd.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace psx::os {

namespace {

std::string nonEmptyEnv(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
}

} // namespace

std::string homeDirectory() {
    if (const std::string home = nonEmptyEnv("HOME"); !home.empty()) {
        return home;
    }
    std::vector<char> buffer(16384);
    passwd entry{};
    passwd* found = nullptr;
    if (::getpwuid_r(::getuid(), &entry, buffer.data(), buffer.size(), &found) == 0 && found != nullptr &&
        found->pw_dir != nullptr) {
        return found->pw_dir;
    }
    return {};
}

std::string stateDirectory(const std::string& application) {
    namespace fs = std::filesystem;
    if (const std::string xdgState = nonEmptyEnv("XDG_STATE_HOME"); !xdgState.empty()) {
        return (fs::path(xdgState) / application).string();
    }
    if (const std::string home = homeDirectory(); !home.empty()) {
        return (fs::path(home) / ".local" / "state" / application).string();
    }
    return application;
}

} // namespace psx::os
