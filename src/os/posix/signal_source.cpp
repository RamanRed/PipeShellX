#include "psx/os/signal_source.hpp"

#include "posix_error.hpp"
#include "sources.hpp"

#include <cerrno>
#include <csignal>
#include <vector>

namespace psx::os {

namespace posix {

int signalNumber(Signal signal) noexcept {
    switch (signal) {
        case Signal::Interrupt:
            return SIGINT;
        case Signal::Terminate:
            return SIGTERM;
        case Signal::Hangup:
            return SIGHUP;
        case Signal::WindowResize:
            return SIGWINCH;
    }
    return 0;
}

bool signalFromNumber(int number, Signal& out) noexcept {
    switch (number) {
        case SIGINT:
            out = Signal::Interrupt;
            return true;
        case SIGTERM:
            out = Signal::Terminate;
            return true;
        case SIGHUP:
            out = Signal::Hangup;
            return true;
        case SIGWINCH:
            out = Signal::WindowResize;
            return true;
        default:
            return false;
    }
}

} // namespace posix

Result<std::unique_ptr<SignalSource>> SignalSource::create(std::initializer_list<Signal> signals) {
    return create(std::vector<Signal>(signals));
}

Result<std::unique_ptr<SignalSource>> SignalSource::create(const std::vector<Signal>& signals) {
    if (signals.empty()) {
        return Error{ErrorClass::InvalidArgument, EINVAL, "signal_source.create"};
    }
    std::vector<int> numbers;
    for (Signal signal : signals) {
        numbers.push_back(posix::signalNumber(signal));
    }
    return posix::createPlatformSignalSource(numbers);
}

} // namespace psx::os
