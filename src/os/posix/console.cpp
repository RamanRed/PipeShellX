#include "psx/os/console.hpp"

#include "posix_error.hpp"

#include <cerrno>
#include <iostream>
#include <termios.h>
#include <unistd.h>

namespace psx::os {

namespace {

int descriptor(StandardStream stream) noexcept {
    switch (stream) {
        case StandardStream::Input:
            return STDIN_FILENO;
        case StandardStream::Output:
            return STDOUT_FILENO;
        case StandardStream::Error:
            return STDERR_FILENO;
    }
    return -1;
}

} // namespace

bool isInteractive(StandardStream stream) noexcept {
    return ::isatty(descriptor(stream)) == 1;
}

Result<std::string> readSecret(const std::string& prompt) {
    if (!isInteractive(StandardStream::Input)) {
        return Error{ErrorClass::Unsupported, ENOTTY, "readSecret"};
    }
    termios original{};
    if (::tcgetattr(STDIN_FILENO, &original) == -1) {
        return posix::fromErrno("tcgetattr", errno);
    }
    termios hidden = original;
    hidden.c_lflag &= static_cast<tcflag_t>(~ECHO);
    if (::tcsetattr(STDIN_FILENO, TCSANOW, &hidden) == -1) {
        return posix::fromErrno("tcsetattr", errno);
    }

    std::cout << prompt;
    std::cout.flush();
    std::string line;
    const bool got = static_cast<bool>(std::getline(std::cin, line));

    (void)::tcsetattr(STDIN_FILENO, TCSANOW, &original);
    std::cout << '\n';
    std::cout.flush();

    if (!got) {
        return Error{ErrorClass::Closed, 0, "readSecret"};
    }
    return line;
}

} // namespace psx::os
