#pragma once

#include "psx/os/socket.hpp"
#include "psx/os/tls.hpp"
#include "psx/result.hpp"
#include "psx/runtime/reactor.hpp"
#include "psx/transport/session.hpp"
#include "psx/transport/tls_stream.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace psx::transport {

// Liveness lease. A silent partition (the peer's host vanishes, the network
// drops, the process freezes) sends no FIN/RST, so a dropped connection is
// invisible to the socket. With a heartbeat armed, each end pings on `interval`
// and treats ANY inbound frame (a PONG, but DATA/EXIT count too) as proof of
// life; after `maxMissed` consecutive silent intervals it fails the transport
// via onError, which fences the node's stages and marks the host lost. Defaults
// (2 s x 3) surface a loss in ~6 s. interval == 0 disables the lease.
struct HeartbeatOptions {
    std::chrono::milliseconds interval{0};
    int maxMissed{3};
};

// The default backplane lease: ping every 2 s and fail after 3 silent intervals
// (~6 s to surface a partition).
inline constexpr HeartbeatOptions kDefaultLease{std::chrono::milliseconds(2000), 3};

// One end of the psx/1 native backplane: a Session multiplexed over a TlsStream
// (mTLS 1.3) on the reactor. Thin wiring — TlsStream delivers decrypted bytes to
// Session::receive(), and the Session's outbound frames go back through
// TlsStream::send(). Open streams only after onReady (the handshake is done).
class NativeTransport {
public:
    struct Callbacks {
        // Authorization: after the handshake authenticates the peer (its cert is
        // CA-signed), this decides whether that IDENTITY is allowed, by its SAN
        // URI. Return false to reject an authenticated-but-unauthorized peer — the
        // transport then fails via onError instead of firing onReady. Unset =
        // accept any authenticated peer (authN only).
        std::function<bool(std::string_view sanUri)> authorize;
        std::function<void()> onReady;           // secured + authorized; safe to open streams
        std::function<void(psx::Error)> onError; // fatal TLS / protocol / authorization / lease error
    };

    NativeTransport(psx::runtime::Reactor& reactor,
                    psx::os::Socket socket,
                    psx::os::Tls tls,
                    Role role,
                    SessionHandler& handler,
                    Callbacks callbacks,
                    std::uint32_t initialWindow = kDefaultStreamWindow,
                    HeartbeatOptions heartbeat = {});
    ~NativeTransport();

    NativeTransport(const NativeTransport&) = delete;
    NativeTransport& operator=(const NativeTransport&) = delete;

    // Registers with the reactor and starts the handshake.
    psx::Result<void> start();

    Session& session() noexcept { return *session_; }
    bool established() const noexcept { return stream_->established(); }
    // The peer's SAN-URI identity (available after onReady).
    std::string peerSanUri() const { return stream_->tls().peerSanUri(); }

private:
    void onData(std::string_view plaintext);
    void fail(const psx::Error& error);
    void armHeartbeat(); // schedules the next lease tick (no-op if disabled)
    void heartbeatTick();

    psx::runtime::Reactor& reactor_;
    std::unique_ptr<Session> session_;
    std::unique_ptr<TlsStream> stream_;
    Callbacks callbacks_;
    HeartbeatOptions heartbeat_;
    psx::runtime::TimerId heartbeatTimer_ = 0;
    bool sawInbound_ = false;
    int missedBeats_ = 0;
    bool failed_ = false;
};

} // namespace psx::transport
