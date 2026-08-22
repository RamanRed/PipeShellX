#pragma once

#include "psx/os/socket.hpp"
#include "psx/os/tls.hpp"
#include "psx/result.hpp"
#include "psx/runtime/reactor.hpp"

#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace psx::transport {

// A TLS-secured byte stream on the reactor over an already-connected socket. It
// drives the mTLS handshake, then delivers decrypted application bytes to onData
// and encrypts+queues plaintext passed to send(). Outbound ciphertext is
// buffered when the socket is not writable (edge-triggered discipline). connect/
// accept is the caller's job; this class assumes a connected os::Socket and an
// os::Tls already in connect/accept state.
class TlsStream {
public:
    struct Callbacks {
        std::function<void()> onReady;                // handshake established
        std::function<void(std::string_view)> onData; // decrypted app bytes
        std::function<void(psx::Error)> onError;      // fatal error (stream is done)
    };

    TlsStream(psx::runtime::Reactor& reactor, psx::os::Socket socket, psx::os::Tls tls, Callbacks callbacks);
    ~TlsStream();
    TlsStream(const TlsStream&) = delete;
    TlsStream& operator=(const TlsStream&) = delete;

    // Registers with the reactor and begins the handshake.
    psx::Result<void> start();

    // Encrypt and queue plaintext for the peer. Valid after onReady.
    void send(std::span<const char> plaintext);

    bool established() const noexcept { return established_; }
    const psx::os::Tls& tls() const noexcept { return tls_; }

private:
    void onEvent(psx::os::Readiness readiness);
    void onReadable();
    void driveHandshake();
    void deliverAppData();
    void flushOutbound();
    void updateInterest();
    void fail(const psx::Error& error);

    psx::runtime::Reactor& reactor_;
    psx::os::Socket socket_;
    psx::os::Tls tls_;
    Callbacks callbacks_;
    psx::runtime::Token token_ = 0;
    std::string outbound_; // ciphertext pending a writable socket
    bool established_ = false;
    bool failed_ = false;
};

} // namespace psx::transport
