#pragma once

// psx::os::SignalSource — delivers operator signals as pollable events
// instead of asynchronous handlers (signalfd on Linux, kqueue EVFILT_SIGNAL on
// Darwin/BSD, SetConsoleCtrlHandler on Windows later). While a source exists
// the subscribed signals no longer take their default action; destruction
// restores the previous dispositions and mask. Create it before spawning
// threads so that every thread inherits the same mask.

#include "psx/os/handle.hpp"
#include "psx/result.hpp"

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <vector>

namespace psx::os {

enum class Signal : std::uint8_t {
    Interrupt,   // SIGINT  / CTRL_C
    Terminate,   // SIGTERM / CTRL_CLOSE
    Hangup,      // SIGHUP
    WindowResize // SIGWINCH
};

class SignalSource {
public:
    static Result<std::unique_ptr<SignalSource>> create(std::initializer_list<Signal> signals);

    virtual ~SignalSource() = default;
    SignalSource(const SignalSource&) = delete;
    SignalSource& operator=(const SignalSource&) = delete;

    // Readable when at least one subscribed signal arrived since the last drain().
    virtual const Handle& handle() const noexcept = 0;

    // Signals received since the last drain(); repeats may be coalesced.
    virtual Result<std::vector<Signal>> drain() = 0;

protected:
    SignalSource() = default;
};

} // namespace psx::os
