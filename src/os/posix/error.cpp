#include "posix_error.hpp"
#include "psx/result.hpp"

#include <cerrno>
#include <cstring>
#include <string>

namespace psx {

const char* toString(ErrorClass cls) noexcept {
    switch (cls) {
        case ErrorClass::Interrupted:
            return "interrupted";
        case ErrorClass::WouldBlock:
            return "would block";
        case ErrorClass::BrokenPipe:
            return "broken pipe";
        case ErrorClass::Closed:
            return "closed";
        case ErrorClass::NotFound:
            return "not found";
        case ErrorClass::PermissionDenied:
            return "permission denied";
        case ErrorClass::TooManyHandles:
            return "too many open handles";
        case ErrorClass::InvalidArgument:
            return "invalid argument";
        case ErrorClass::NoSuchProcess:
            return "no such process";
        case ErrorClass::Timeout:
            return "timed out";
        case ErrorClass::Unsupported:
            return "unsupported";
        case ErrorClass::Other:
            return "error";
    }
    return "error";
}

std::string Error::message() const {
    std::string text = std::string(op) + ": " + toString(cls);
    if (code != 0) {
        char buffer[128] = {};
        // GNU strerror_r may return a pointer to a static string instead of
        // filling `buffer`; the XSI variant returns int. Handle both.
        const char* platformText = nullptr;
#if defined(__GLIBC__) && defined(_GNU_SOURCE)
        platformText = ::strerror_r(code, buffer, sizeof(buffer));
#else
        platformText = ::strerror_r(code, buffer, sizeof(buffer)) == 0 ? buffer : "";
#endif
        text += " (";
        if (platformText != nullptr && *platformText != '\0') {
            text += platformText;
            text += ", ";
        }
        text += "code " + std::to_string(code) + ")";
    }
    return text;
}

namespace os::posix {

ErrorClass classify(int err) noexcept {
    switch (err) {
        case EINTR:
            return ErrorClass::Interrupted;
        case EAGAIN:
#if EWOULDBLOCK != EAGAIN
        case EWOULDBLOCK:
#endif
            return ErrorClass::WouldBlock;
        case EPIPE:
            return ErrorClass::BrokenPipe;
        case EBADF:
            return ErrorClass::Closed;
        case ENOENT:
            return ErrorClass::NotFound;
        case EACCES:
        case EPERM:
            return ErrorClass::PermissionDenied;
        case EMFILE:
        case ENFILE:
            return ErrorClass::TooManyHandles;
        case EINVAL:
            return ErrorClass::InvalidArgument;
        case ESRCH:
        case ECHILD:
            return ErrorClass::NoSuchProcess;
        case ETIMEDOUT:
            return ErrorClass::Timeout;
        case ENOSYS:
#if defined(ENOTSUP)
        case ENOTSUP:
#endif
#if defined(EOPNOTSUPP) && EOPNOTSUPP != ENOTSUP
        case EOPNOTSUPP:
#endif
            return ErrorClass::Unsupported;
        default:
            return ErrorClass::Other;
    }
}

} // namespace os::posix
} // namespace psx
