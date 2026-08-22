#pragma once

#include "psx/os/socket.hpp"
#include "psx/os/tls.hpp"
#include "psx/result.hpp"
#include "psx/runtime/reactor.hpp"
#include "psx/transport/session.hpp"
#include "psx/transport/tls_stream.hpp"

#include <functional>
#include <memory>
#include <string>

namespace psx::transport {

// One end of the psx/1 native backplane: a Session multiplexed over a TlsStream
// (mTLS 1.3) on the reactor. Thin wiring — TlsStream delivers decrypted bytes to
// Session::receive(), and the Session's outbound frames go back through
// TlsStream::send(). Open streams only after onReady (the handshake is done).
class NativeTransport {
public:
    struct Callbacks {
        std::function<void()> onReady;           // secured; identity available, safe to open streams
        std::function<void(psx::Error)> onError; // fatal TLS or protocol error; the transport is done
    };

    NativeTransport(psx::runtime::Reactor& reactor,
                    psx::os::Socket socket,
                    psx::os::Tls tls,
                    Role role,
                    SessionHandler& handler,
                    Callbacks callbacks);

    // Registers with the reactor and starts the handshake.
    psx::Result<void> start();

    Session& session() noexcept { return *session_; }
    bool established() const noexcept { return stream_->established(); }
    // The peer's SAN-URI identity (available after onReady).
    std::string peerSanUri() const { return stream_->tls().peerSanUri(); }

private:
    void onData(std::string_view plaintext);
    void fail(const psx::Error& error);

    std::unique_ptr<Session> session_;
    std::unique_ptr<TlsStream> stream_;
    Callbacks callbacks_;
    bool failed_ = false;
};

} // namespace psx::transport
