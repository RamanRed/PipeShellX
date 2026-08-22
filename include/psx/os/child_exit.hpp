#pragma once

// psx::os::ChildExitSource — one pollable handle that becomes readable when
// any watched child has exited. The source never reaps: drain() returns the
// exited ProcessIds once, and their owners call Process::wait()/tryWait().

#include "psx/os/handle.hpp"
#include "psx/os/process.hpp"
#include "psx/result.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace psx::os {

enum class ChildExitMode : std::uint8_t {
    Auto,
    Native,      // pidfd + epoll (Linux ≥ 5.3), kqueue EVFILT_PROC (Darwin/BSD)
    SignalDriven // SIGCHLD → self-pipe, exits confirmed with waitid(WNOWAIT); portable
};

class ChildExitSource {
public:
    static bool available(ChildExitMode mode) noexcept;
    static Result<std::unique_ptr<ChildExitSource>> create(ChildExitMode mode = ChildExitMode::Auto);

    virtual ~ChildExitSource() = default;
    ChildExitSource(const ChildExitSource&) = delete;
    ChildExitSource& operator=(const ChildExitSource&) = delete;

    virtual ChildExitMode mode() const noexcept = 0;

    // Readable (Poller Interest::Readable) when at least one exit is pending.
    virtual const Handle& handle() const noexcept = 0;

    // `pid` must be an unreaped child of this process; a child that already
    // exited is reported on the next drain(). InvalidArgument if watched twice.
    virtual Result<void> watch(ProcessId pid) = 0;
    virtual Result<void> unwatch(ProcessId pid) = 0;

    // Children that exited since the last drain(); each reported exactly once
    // and no longer watched afterwards. Clears the handle's readiness.
    virtual Result<std::vector<ProcessId>> drain() = 0;

    virtual std::size_t size() const noexcept = 0;

protected:
    ChildExitSource() = default;
};

} // namespace psx::os
