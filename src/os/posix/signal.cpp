#include "psx/os/io.hpp"

#include "posix_error.hpp"

#include <cerrno>
#include <signal.h>

namespace psx::os {

Result<void> ignoreBrokenPipeSignal() {
    struct sigaction action{};
    action.sa_handler = SIG_IGN;
    sigemptyset(&action.sa_mask); // a macro on some libcs, so no :: qualifier
    if (::sigaction(SIGPIPE, &action, nullptr) == -1) {
        return posix::fromErrno("sigaction(SIGPIPE)", errno);
    }
    return {};
}

} // namespace psx::os
