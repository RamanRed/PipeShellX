#include "psx/runtime/reactor.hpp"

#include "psx/os/io.hpp"

#include <algorithm>
#include <cstdlib>
#include <string_view>
#include <utility>

namespace psx::runtime {

namespace {

constexpr std::size_t kEventBatch = 256;

} // namespace

os::Poller::Backend Reactor::backendFromEnvironment() {
    const char* value = std::getenv("PIPESHELLX_POLLER");
    const std::string_view choice = value != nullptr ? value : "";
    if (choice == "poll") {
        return os::Poller::Backend::Poll;
    }
    if (choice == "epoll") {
        return os::Poller::Backend::Epoll;
    }
    if (choice == "kqueue") {
        return os::Poller::Backend::Kqueue;
    }
    return os::Poller::Backend::Auto;
}

Result<std::unique_ptr<Reactor>> Reactor::create(const Options& options) {
    // The reactor writes to child stdin pipes; a child that closes its read end
    // must yield BrokenPipe, not SIGPIPE (which would kill the process).
    PSX_TRY(os::ignoreBrokenPipeSignal());

    std::unique_ptr<Reactor> reactor(new Reactor());

    auto poller = os::Poller::create(options.backend);
    if (!poller.ok()) {
        return poller.error();
    }
    reactor->poller_ = std::move(poller.value());

    auto children = os::ChildExitSource::create(options.childExit);
    if (!children.ok()) {
        return children.error();
    }
    reactor->children_ = std::move(children.value());
    reactor->childToken_ = reactor->nextToken_++;
    PSX_TRY(reactor->poller_->add(reactor->children_->handle(), os::Interest::Readable, reactor->childToken_));

    if (!options.signals.empty()) {
        auto signals = os::SignalSource::create(options.signals);
        if (!signals.ok()) {
            return signals.error();
        }
        reactor->signals_ = std::move(signals.value());
        reactor->signalToken_ = reactor->nextToken_++;
        PSX_TRY(reactor->poller_->add(reactor->signals_->handle(), os::Interest::Readable, reactor->signalToken_));
    }

    reactor->events_.resize(kEventBatch);
    return reactor;
}

Result<std::unique_ptr<Reactor>> Reactor::create() {
    return create(Options{});
}

os::Poller::Backend Reactor::backend() const noexcept {
    return poller_->backend();
}

Result<Token> Reactor::watch(const os::Handle& handle, os::Interest interest, IoHandler handler) {
    // The drain-until-WouldBlock discipline the handlers rely on requires a
    // non-blocking handle; establish it here so no call site can forget (a
    // no-op for the future completion-style backend).
    PSX_TRY(const_cast<os::Handle&>(handle).setNonBlocking(true));
    const Token token = nextToken_++;
    PSX_TRY(poller_->add(handle, interest, token));
    io_.emplace(token, std::move(handler));
    return token;
}

Result<void> Reactor::modify(Token token, os::Interest interest) {
    if (io_.count(token) == 0) {
        return Error{ErrorClass::NotFound, 0, "reactor.modify"};
    }
    return poller_->modify(token, interest);
}

Result<void> Reactor::unwatch(Token token) {
    if (io_.erase(token) == 0) {
        return Error{ErrorClass::NotFound, 0, "reactor.unwatch"};
    }
    return poller_->remove(token);
}

TimerId Reactor::after(std::chrono::milliseconds delay, TimerHandler handler) {
    const TimerId id = nextTimer_++;
    const auto deadline = std::chrono::steady_clock::now() + std::max(delay, std::chrono::milliseconds(0));
    timers_.emplace(id, std::move(handler));
    timerQueue_.push(TimerEntry{deadline, id});
    return id;
}

bool Reactor::cancel(TimerId timer) {
    return timers_.erase(timer) != 0; // the queue entry is skipped lazily
}

Result<void> Reactor::watchChild(os::ProcessId pid, ChildHandler handler) {
    if (childHandlers_.count(pid) != 0) {
        return Error{ErrorClass::InvalidArgument, 0, "reactor.watchChild"};
    }
    PSX_TRY(children_->watch(pid));
    childHandlers_.emplace(pid, std::move(handler));
    return {};
}

Result<void> Reactor::unwatchChild(os::ProcessId pid) {
    if (childHandlers_.erase(pid) == 0) {
        return Error{ErrorClass::NotFound, 0, "reactor.unwatchChild"};
    }
    return children_->unwatch(pid);
}

Result<void> Reactor::onSignal(SignalHandler handler) {
    if (!signals_) {
        return Error{ErrorClass::Unsupported, 0, "reactor.onSignal"};
    }
    signalHandler_ = std::move(handler);
    return {};
}

std::optional<std::chrono::milliseconds> Reactor::waitBudget(std::optional<std::chrono::milliseconds> timeout) {
    // Drop cancelled timers from the head so that they do not shorten the wait.
    while (!timerQueue_.empty() && timers_.count(timerQueue_.top().id) == 0) {
        timerQueue_.pop();
    }
    std::optional<std::chrono::milliseconds> budget = timeout;
    if (!timerQueue_.empty()) {
        const auto now = std::chrono::steady_clock::now();
        const auto until = timerQueue_.top().deadline <= now
                               ? std::chrono::milliseconds(0)
                               : std::chrono::ceil<std::chrono::milliseconds>(timerQueue_.top().deadline - now);
        budget = budget ? std::min(*budget, until) : until;
    }
    return budget;
}

Result<void> Reactor::runOnce(std::optional<std::chrono::milliseconds> timeout) {
    auto ready = poller_->wait(events_, waitBudget(timeout));
    if (!ready.ok()) {
        return ready.error();
    }

    for (std::size_t i = 0; i < ready.value(); ++i) {
        const os::Event event = events_[i];
        if (event.token == childToken_) {
            dispatchChildren();
        } else if (signals_ && event.token == signalToken_) {
            dispatchSignals();
        } else if (auto it = io_.find(event.token); it != io_.end()) {
            // Copy: the handler may unwatch itself (destroying the stored callable).
            IoHandler handler = it->second;
            handler(event.readiness);
        }
    }

    fireDueTimers();
    return {};
}

void Reactor::dispatchChildren() {
    auto exited = children_->drain();
    if (!exited.ok()) {
        return;
    }
    for (os::ProcessId pid : exited.value()) {
        auto it = childHandlers_.find(pid);
        if (it == childHandlers_.end()) {
            continue;
        }
        ChildHandler handler = std::move(it->second);
        childHandlers_.erase(it);
        handler(pid);
    }
}

void Reactor::dispatchSignals() {
    auto received = signals_->drain();
    if (!received.ok()) {
        return;
    }
    for (os::Signal signal : received.value()) {
        if (signalHandler_) {
            SignalHandler handler = signalHandler_;
            handler(signal);
        }
    }
}

void Reactor::fireDueTimers() {
    const auto now = std::chrono::steady_clock::now();
    std::vector<TimerHandler> due;
    while (!timerQueue_.empty() && timerQueue_.top().deadline <= now) {
        const TimerId id = timerQueue_.top().id;
        timerQueue_.pop();
        auto it = timers_.find(id);
        if (it != timers_.end()) {
            due.push_back(std::move(it->second));
            timers_.erase(it);
        }
    }
    for (auto& handler : due) {
        handler();
    }
}

Result<void> Reactor::run() {
    if (stopRequested_.exchange(false)) {
        return {};
    }
    while (!stopRequested_.load(std::memory_order_acquire)) {
        PSX_TRY(runOnce(std::nullopt));
    }
    stopRequested_.store(false, std::memory_order_release);
    return {};
}

void Reactor::stop() noexcept {
    stopRequested_.store(true, std::memory_order_release);
    (void)poller_->wake();
}

Result<void> Reactor::wake() {
    return poller_->wake();
}

} // namespace psx::runtime
