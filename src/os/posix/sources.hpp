#pragma once

// Internal to src/os/posix: platform factories for the event sources.

#include "psx/os/child_exit.hpp"
#include "psx/os/signal_source.hpp"

#include <csignal>
#include <memory>
#include <vector>

namespace psx::os::posix {

bool nativeChildExitAvailable() noexcept;
Result<std::unique_ptr<ChildExitSource>> createNativeChildExitSource();
Result<std::unique_ptr<ChildExitSource>> createSignalDrivenChildExitSource();

Result<std::unique_ptr<SignalSource>> createPlatformSignalSource(const std::vector<int>& signalNumbers);

int signalNumber(Signal signal) noexcept;
bool signalFromNumber(int number, Signal& out) noexcept;

// True when `pid` is an exited-but-unreaped child (waitid WNOWAIT). Sets
// `exists` to false when it is not a child of this process at all.
bool childHasExited(ProcessId pid, bool& exists) noexcept;

} // namespace psx::os::posix
