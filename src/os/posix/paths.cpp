#include "psx/os/paths.hpp"

#include "posix_error.hpp"

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <pwd.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace psx::os {

namespace {

std::string nonEmptyEnv(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
}

std::string accountHomeDirectory(uid_t user) {
    std::vector<char> buffer(16384);
    passwd entry{};
    passwd* found = nullptr;
    if (::getpwuid_r(user, &entry, buffer.data(), buffer.size(), &found) == 0 && found != nullptr &&
        found->pw_dir != nullptr) {
        return found->pw_dir;
    }
    return {};
}

Result<void>
atomicWriteWithMode(const std::string& path, std::string_view contents, mode_t mode, const char* operation) {
    namespace fs = std::filesystem;
    const fs::path target(path);
    if (target.filename().empty()) {
        return Error{ErrorClass::InvalidArgument, EINVAL, operation};
    }
    const fs::path parent = target.parent_path().empty() ? fs::path(".") : target.parent_path();
    std::string pattern = (parent / ("." + target.filename().string() + ".tmp.XXXXXX")).string();
    std::vector<char> temporaryName(pattern.begin(), pattern.end());
    temporaryName.push_back('\0');
    int descriptor = ::mkstemp(temporaryName.data());
    if (descriptor < 0) {
        return posix::fromErrno(operation, errno);
    }

    auto failAndClean = [&](const char* failedOperation, int code) -> Result<void> {
        if (descriptor >= 0) {
            (void)::close(descriptor);
            descriptor = -1;
        }
        (void)::unlink(temporaryName.data());
        return posix::fromErrno(failedOperation, code);
    };

    // mkstemp creates with 0600 (possibly further restricted by umask). Set the
    // exact requested mode before writing, so secret content is never present
    // in a group/world-readable inode.
    if (::fchmod(descriptor, mode) != 0) {
        return failAndClean("chmod temporary file", errno);
    }
    while (!contents.empty()) {
        const ssize_t written = ::write(descriptor, contents.data(), contents.size());
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return failAndClean("write temporary file", written < 0 ? errno : EIO);
        }
        contents.remove_prefix(static_cast<std::size_t>(written));
    }
    if (::fsync(descriptor) != 0) {
        return failAndClean("sync temporary file", errno);
    }
    if (::close(descriptor) != 0) {
        const int code = errno;
        descriptor = -1;
        (void)::unlink(temporaryName.data());
        return posix::fromErrno("close temporary file", code);
    }
    descriptor = -1;
    if (::rename(temporaryName.data(), path.c_str()) != 0) {
        const int code = errno;
        (void)::unlink(temporaryName.data());
        return posix::fromErrno("replace file", code);
    }
    return {};
}

} // namespace

bool environmentPathsAreTrusted() noexcept {
    return detail::credentialsPermitEnvironment(
        static_cast<std::uintmax_t>(::getuid()), static_cast<std::uintmax_t>(::geteuid()),
        static_cast<std::uintmax_t>(::getgid()), static_cast<std::uintmax_t>(::getegid()));
}

std::string homeDirectory() {
    if (environmentPathsAreTrusted()) {
        if (const std::string home = nonEmptyEnv("HOME"); !home.empty()) {
            return home;
        }
    }
    return accountHomeDirectory(::geteuid());
}

std::string stateDirectory(const std::string& application) {
    namespace fs = std::filesystem;
    if (environmentPathsAreTrusted()) {
        if (const std::string xdgState = nonEmptyEnv("XDG_STATE_HOME"); !xdgState.empty()) {
            return (fs::path(xdgState) / application).string();
        }
    }
    if (const std::string home = homeDirectory(); !home.empty()) {
        return (fs::path(home) / ".local" / "state" / application).string();
    }
    return application;
}

Result<void> atomicRewriteFile(const std::string& path, std::string_view contents) {
    mode_t existingMode = 0600;
    struct stat targetStatus{};
    if (::stat(path.c_str(), &targetStatus) == 0) {
        existingMode = targetStatus.st_mode & 0777;
    } else if (errno != ENOENT) {
        return posix::fromErrno("stat inventory", errno);
    }

    return atomicWriteWithMode(path, contents, existingMode, "atomic inventory rewrite");
}

Result<void> atomicWritePrivateFile(const std::string& path, std::string_view contents) {
    return atomicWriteWithMode(path, contents, 0600, "atomic private file write");
}

} // namespace psx::os
