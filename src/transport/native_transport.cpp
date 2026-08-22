#include "psx/transport/native_transport.hpp"

#include <span>
#include <utility>

namespace psx::transport {

NativeTransport::NativeTransport(psx::runtime::Reactor& reactor,
                                 psx::os::Socket socket,
                                 psx::os::Tls tls,
                                 Role role,
                                 SessionHandler& handler,
                                 Callbacks callbacks)
    : callbacks_(std::move(callbacks)) {
    // The Session's outbound frames are encrypted and sent by the TlsStream. The
    // capture is safe: the write callback only fires when the Session sends, which
    // is after onReady, by when stream_ is set.
    session_ = std::make_unique<Session>(
        role, [this](std::string_view frame) { stream_->send(std::span<const char>(frame.data(), frame.size())); },
        handler);
    stream_ = std::make_unique<TlsStream>(
        reactor, std::move(socket), std::move(tls),
        TlsStream::Callbacks{.onReady =
                                 [this] {
                                     if (callbacks_.onReady) {
                                         callbacks_.onReady();
                                     }
                                 },
                             .onData = [this](std::string_view plaintext) { onData(plaintext); },
                             .onError = [this](const psx::Error& error) { fail(error); }});
}

psx::Result<void> NativeTransport::start() {
    return stream_->start();
}

void NativeTransport::onData(std::string_view plaintext) {
    if (failed_) {
        return;
    }
    if (auto received = session_->receive(plaintext); !received.ok()) {
        fail(received.error()); // a protocol violation ends the connection
    }
}

void NativeTransport::fail(const psx::Error& error) {
    if (failed_) {
        return;
    }
    failed_ = true;
    if (callbacks_.onError) {
        callbacks_.onError(error);
    }
}

} // namespace psx::transport
