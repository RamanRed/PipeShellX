#pragma once

// psx::Result<T> — expected-style return type used by L0–L2 (ADR-005).
// Public headers under include/psx/ use std:: types only.

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace psx {

// Portable failure classes; the raw platform code travels alongside.
enum class ErrorClass : std::uint8_t {
    Interrupted,      // EINTR
    WouldBlock,       // EAGAIN / EWOULDBLOCK
    BrokenPipe,       // EPIPE / ERROR_BROKEN_PIPE
    Closed,           // EBADF or an invalid handle
    NotFound,         // ENOENT
    PermissionDenied, // EACCES / EPERM
    TooManyHandles,   // EMFILE / ENFILE
    InvalidArgument,  // EINVAL
    NoSuchProcess,    // ESRCH / ECHILD
    Timeout,          // ETIMEDOUT or a deadline
    Unsupported,      // ENOSYS / ENOTSUP, or not available on this platform
    Other
};

const char* toString(ErrorClass cls) noexcept;

struct Error {
    ErrorClass cls = ErrorClass::Other;
    int code = 0;        // errno / GetLastError(); 0 when not platform-originated
    const char* op = ""; // operation name; must point to static storage

    // "<op>: <class> (<platform text>, code N)" — for logs and user messages.
    std::string message() const;
};

template <class T> class [[nodiscard]] Result {
public:
    Result(T value) : value_(std::move(value)) {}
    Result(Error error) noexcept : error_(error) {}

    bool ok() const noexcept { return value_.has_value(); }
    explicit operator bool() const noexcept { return ok(); }

    T& value() & noexcept { return *value_; }
    const T& value() const& noexcept { return *value_; }
    T&& value() && noexcept { return std::move(*value_); }
    T* operator->() noexcept { return &*value_; }
    const T* operator->() const noexcept { return &*value_; }

    const Error& error() const noexcept { return error_; }

private:
    std::optional<T> value_;
    Error error_{};
};

template <> class [[nodiscard]] Result<void> {
public:
    Result() noexcept = default; // success
    Result(Error error) noexcept : ok_(false), error_(error) {}

    bool ok() const noexcept { return ok_; }
    explicit operator bool() const noexcept { return ok_; }
    const Error& error() const noexcept { return error_; }

private:
    bool ok_ = true;
    Error error_{};
};

} // namespace psx

// Propagate a failed Result from the enclosing function (which must itself
// return a psx::Result). Usable with Result<T> and Result<void>.
#define PSX_TRY(expr)                                                                                                  \
    do {                                                                                                               \
        auto&& psx_try_result_ = (expr);                                                                               \
        if (!psx_try_result_.ok()) {                                                                                   \
            return psx_try_result_.error();                                                                            \
        }                                                                                                              \
    } while (0)
