#pragma once

// psx::os::Poller — readiness demultiplexer over Handles, keyed by caller
// tokens. Backends: poll (portable, level-triggered), kqueue (EV_CLEAR) and
// epoll (EPOLLET). Consumers must drain a ready handle until WouldBlock so
// that all three backends behave identically (edge-triggered discipline).

#include "psx/os/handle.hpp"
#include "psx/result.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace psx::os {

enum class Interest : std::uint8_t { None = 0, Readable = 1, Writable = 2 };
enum class Readiness : std::uint8_t { None = 0, Readable = 1, Writable = 2, Hangup = 4, Error = 8 };

constexpr Readiness operator|(Readiness a, Readiness b) noexcept {
    return static_cast<Readiness>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}
constexpr bool has(Interest set, Interest flag) noexcept {
    return (static_cast<std::uint8_t>(set) & static_cast<std::uint8_t>(flag)) != 0;
}
constexpr bool has(Readiness set, Readiness flag) noexcept {
    return (static_cast<std::uint8_t>(set) & static_cast<std::uint8_t>(flag)) != 0;
}

struct Event {
    std::uint64_t token = 0;
    Readiness readiness = Readiness::None;
};

class Poller {
public:
    enum class Backend : std::uint8_t { Auto, Poll, Epoll, Kqueue };

    static bool available(Backend backend) noexcept;
    // Auto picks epoll on Linux, kqueue on Darwin/BSD, poll elsewhere.
    static Result<std::unique_ptr<Poller>> create(Backend preferred = Backend::Auto);

    virtual ~Poller() = default;
    Poller(const Poller&) = delete;
    Poller& operator=(const Poller&) = delete;

    virtual Backend backend() const noexcept = 0;

    // `token` must be unique per poller; Interest::None keeps the
    // registration but reports nothing until modify() re-arms it, at which
    // point already-pending readiness is reported again.
    virtual Result<void> add(const Handle& handle, Interest interest, std::uint64_t token) = 0;
    virtual Result<void> modify(std::uint64_t token, Interest interest) = 0;
    virtual Result<void> remove(std::uint64_t token) = 0;

    // Blocks until at least one event, the timeout (nullopt = forever), a
    // wake(), or a signal; returns the number of events written. A wake-up
    // or an interrupted call yields 0 events.
    virtual Result<std::size_t> wait(std::span<Event> events, std::optional<std::chrono::milliseconds> timeout) = 0;

    // Thread-safe: makes the current or next wait() return promptly.
    virtual Result<void> wake() = 0;

    virtual std::size_t size() const noexcept = 0;

protected:
    Poller() = default;
};

} // namespace psx::os
