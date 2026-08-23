#include "psx/transport/native_controller.hpp"

#include "psx/os/socket.hpp"
#include "psx/transport/session.hpp"

#include <cstdint>
#include <utility>

namespace psx::transport {

struct NativeController::Conn : SessionHandler {
    NativeController* owner = nullptr;
    std::size_t index = 0;
    Session* session = nullptr; // bound after the transport is built (for consume/open)
    HostResult result;
    bool done = false;
    std::unique_ptr<NativeTransport> transport; // declared last: destroyed first

    void onData(StreamId id, std::string_view data, bool /*endStream*/, Channel channel) override {
        if (session != nullptr) {
            session->consume(id, static_cast<std::uint32_t>(data.size())); // grant credit
        }
        result.output.append(data);
        if (owner->onOutput_) {
            owner->onOutput_(result.host, data, channel);
        }
    }
    void onExit(StreamId /*id*/, const psx::os::ExitStatus& status) override {
        result.ok = true;
        result.exitCode = (status.kind == psx::os::ExitStatus::Kind::Exited) ? status.code : -1;
        finish();
    }
    void fail(const std::string& message) {
        if (!done) {
            result.error = message;
            result.ok = false;
        }
        finish();
    }
    void finish() {
        if (done) {
            return;
        }
        done = true;
        owner->onConnDone(index);
    }
};

NativeController::NativeController(psx::runtime::Reactor& reactor,
                                   psx::os::TlsConfig controllerConfig,
                                   OnOutput onOutput)
    : reactor_(reactor), config_(std::move(controllerConfig)), onOutput_(std::move(onOutput)) {
    config_.isServer = false;
}

NativeController::~NativeController() = default;

psx::Result<void> NativeController::start(const std::vector<Target>& targets,
                                          const std::vector<std::string>& command,
                                          std::function<void(std::vector<HostResult>)> onComplete) {
    command_ = command;
    onComplete_ = std::move(onComplete);
    remaining_ = targets.size();
    conns_.reserve(targets.size());

    if (targets.empty()) {
        completed_ = true;
        if (onComplete_) {
            onComplete_({});
        }
        return {};
    }

    for (std::size_t i = 0; i < targets.size(); ++i) {
        const Target& target = targets[i];
        auto conn = std::make_unique<Conn>();
        conn->owner = this;
        conn->index = i;
        conn->result.host = target.host;
        Conn* raw = conn.get();
        conns_.push_back(std::move(conn));

        auto socket = psx::os::Socket::connect(target.host, target.port);
        if (!socket.ok()) {
            raw->fail("connect: " + socket.error().message());
            continue;
        }
        auto tls = psx::os::Tls::create(config_);
        if (!tls.ok()) {
            raw->fail("tls: " + tls.error().message());
            continue;
        }

        std::function<bool(std::string_view)> authorize;
        if (!target.expectedSan.empty()) {
            authorize = [san = target.expectedSan](std::string_view s) { return std::string(s) == san; };
        }
        raw->transport = std::make_unique<NativeTransport>(
            reactor_, std::move(socket.value()), std::move(tls.value()), Role::Controller, *raw,
            NativeTransport::Callbacks{.authorize = std::move(authorize),
                                       .onReady = [this, raw] { raw->session->open({.argv = command_}); },
                                       .onError = [raw](const psx::Error& e) { raw->fail(e.message()); }},
            kDefaultStreamWindow, kDefaultLease);
        raw->session = &raw->transport->session();
        if (auto started = raw->transport->start(); !started.ok()) {
            raw->fail("start: " + started.error().message());
        }
    }
    return {};
}

void NativeController::cancel(const std::string& reason) {
    for (auto& conn : conns_) {
        if (!conn->done) {
            conn->fail(reason);
        }
    }
}

void NativeController::onConnDone(std::size_t /*index*/) {
    if (remaining_ > 0) {
        --remaining_;
    }
    if (remaining_ == 0 && !completed_) {
        completed_ = true;
        std::vector<HostResult> results;
        results.reserve(conns_.size());
        for (auto& conn : conns_) {
            results.push_back(std::move(conn->result));
        }
        if (onComplete_) {
            onComplete_(std::move(results));
        }
    }
}

} // namespace psx::transport
