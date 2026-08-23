#include "psx/transport/native_transport.hpp"

#include <span>
#include <utility>

namespace psx::transport {

NativeTransport::NativeTransport(psx::runtime::Reactor& reactor,
                                 psx::os::Socket socket,
                                 psx::os::Tls tls,
                                 Role role,
                                 SessionHandler& handler,
                                 Callbacks callbacks,
                                 std::uint32_t initialWindow,
                                 HeartbeatOptions heartbeat)
    : reactor_(reactor), callbacks_(std::move(callbacks)), heartbeat_(heartbeat) {
    // The Session's outbound frames are encrypted and sent by the TlsStream. The
    // capture is safe: the write callback only fires when the Session sends, which
    // is after onReady, by when stream_ is set.
    session_ = std::make_unique<Session>(
        role, [this](std::string_view frame) { stream_->send(std::span<const char>(frame.data(), frame.size())); },
        handler, initialWindow);
    stream_ = std::make_unique<TlsStream>(
        reactor, std::move(socket), std::move(tls),
        TlsStream::Callbacks{.onReady =
                                 [this] {
                                     if (callbacks_.authorize && !callbacks_.authorize(stream_->tls().peerSanUri())) {
                                         fail(psx::Error{psx::ErrorClass::Other, 0, "peer SAN-URI not authorized"});
                                         return;
                                     }
                                     // The handshake just completed: the peer is
                                     // live, so start the lease from a clean slate.
                                     sawInbound_ = true;
                                     armHeartbeat();
                                     if (callbacks_.onReady) {
                                         callbacks_.onReady();
                                     }
                                 },
                             .onData = [this](std::string_view plaintext) { onData(plaintext); },
                             .onError = [this](const psx::Error& error) { fail(error); }});
}

NativeTransport::~NativeTransport() {
    if (heartbeatTimer_ != 0) {
        (void)reactor_.cancel(heartbeatTimer_);
    }
}

psx::Result<void> NativeTransport::start() {
    return stream_->start();
}

void NativeTransport::onData(std::string_view plaintext) {
    if (failed_) {
        return;
    }
    sawInbound_ = true; // any inbound frame proves the peer is alive (lease)
    if (auto received = session_->receive(plaintext); !received.ok()) {
        fail(received.error()); // a protocol violation ends the connection
    }
}

void NativeTransport::armHeartbeat() {
    if (heartbeat_.interval <= std::chrono::milliseconds(0)) {
        return; // lease disabled
    }
    heartbeatTimer_ = reactor_.after(heartbeat_.interval, [this] { heartbeatTick(); });
}

void NativeTransport::heartbeatTick() {
    heartbeatTimer_ = 0; // the one-shot timer has fired
    if (failed_) {
        return;
    }
    if (sawInbound_) {
        missedBeats_ = 0;
    } else if (++missedBeats_ >= heartbeat_.maxMissed) {
        fail(psx::Error{psx::ErrorClass::Timeout, 0, "peer unresponsive (lease expired)"});
        return; // do not re-arm; the connection is being torn down
    }
    sawInbound_ = false;
    session_->ping(); // elicit a PONG so a quiet-but-live peer still shows activity
    armHeartbeat();
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
