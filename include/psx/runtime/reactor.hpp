#pragma once

// psx::runtime::Reactor — single-threaded event loop (L1) composed from the
// L0 primitives: a Poller for handles, a ChildExitSource, an optional
// SignalSource, and a timer queue. Handlers run on the thread that calls
// run()/runOnce() and may re-enter the reactor (watch/unwatch/after/cancel/
// stop) freely. stop() and wake() are the only thread-safe entry points.

#include "psx/os/child_exit.hpp"
#include "psx/os/handle.hpp"
#include "psx/os/poller.hpp"
#include "psx/os/process.hpp"
#include "psx/os/signal_source.hpp"
#include "psx/result.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <queue>
#include <unordered_map>
#include <vector>

namespace psx::runtime {

using Token = std::uint64_t;
using TimerId = std::uint64_t;

class Reactor {
public:
    using IoHandler = std::function<void(os::Readiness)>;
    using TimerHandler = std::function<void()>;
    using ChildHandler = std::function<void(os::ProcessId)>;
    using SignalHandler = std::function<void(os::Signal)>;

    struct Options {
        os::Poller::Backend backend = os::Poller::Backend::Auto;
        os::ChildExitMode childExit = os::ChildExitMode::Auto;
        std::vector<os::Signal> signals; // empty: no SignalSource, onSignal() is Unsupported
    };

    static Result<std::unique_ptr<Reactor>> create(const Options& options);
    static Result<std::unique_ptr<Reactor>> create(); // default Options

    ~Reactor() = default;
    Reactor(const Reactor&) = delete;
    Reactor& operator=(const Reactor&) = delete;

    // The backend actually in use (never Auto).
    os::Poller::Backend backend() const noexcept;

    // The handle must stay open until unwatch(); non-blocking handles drain
    // until WouldBlock inside the handler (edge-triggered discipline).
    Result<Token> watch(const os::Handle& handle, os::Interest interest, IoHandler handler);
    Result<void> modify(Token token, os::Interest interest);
    Result<void> unwatch(Token token);

    // One-shot timer; fires on the first dispatch round at or after the deadline.
    TimerId after(std::chrono::milliseconds delay, TimerHandler handler);
    bool cancel(TimerId timer);

    // The handler runs once when the child exits and is then forgotten; the
    // owner reaps (Process::tryWait) inside it.
    Result<void> watchChild(os::ProcessId pid, ChildHandler handler);
    Result<void> unwatchChild(os::ProcessId pid);

    Result<void> onSignal(SignalHandler handler);

    // One dispatch round: wait (bounded by `timeout` and the next timer), then
    // run I/O, child-exit, signal and due-timer handlers.
    Result<void> runOnce(std::optional<std::chrono::milliseconds> timeout = std::nullopt);
    // Rounds until stop(); a stop() issued beforehand returns immediately.
    Result<void> run();
    void stop() noexcept;
    Result<void> wake();

    std::size_t pendingTimers() const noexcept { return timers_.size(); }
    std::size_t watchedHandles() const noexcept { return io_.size(); }

private:
    struct TimerEntry {
        std::chrono::steady_clock::time_point deadline;
        TimerId id;
        bool operator>(const TimerEntry& other) const noexcept {
            return deadline > other.deadline || (deadline == other.deadline && id > other.id);
        }
    };

    Reactor() = default;

    std::optional<std::chrono::milliseconds> waitBudget(std::optional<std::chrono::milliseconds> timeout);
    void dispatchChildren();
    void dispatchSignals();
    void fireDueTimers();

    std::unique_ptr<os::Poller> poller_;
    std::unique_ptr<os::ChildExitSource> children_;
    std::unique_ptr<os::SignalSource> signals_;
    std::unordered_map<Token, IoHandler> io_;
    std::unordered_map<TimerId, TimerHandler> timers_;
    std::priority_queue<TimerEntry, std::vector<TimerEntry>, std::greater<TimerEntry>> timerQueue_;
    std::unordered_map<os::ProcessId, ChildHandler> childHandlers_;
    SignalHandler signalHandler_;
    std::vector<os::Event> events_;
    Token nextToken_ = 1;
    TimerId nextTimer_ = 1;
    std::atomic<bool> stopRequested_{false};
};

} // namespace psx::runtime
