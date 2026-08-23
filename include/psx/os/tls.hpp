#pragma once

#include "psx/result.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <string>

namespace psx::os {

// mTLS material (PEM). Mutual auth requires all three.
struct TlsConfig {
    std::string certificatePem; // this endpoint's certificate chain
    std::string privateKeyPem;  // its private key
    std::string caPem;          // the CA the peer's certificate must chain to
    std::string crlPem;         // optional CRL: reject a peer cert it revokes (empty = no CRL check)
    bool isServer = false;
};

// A TLS 1.3 engine driven entirely through memory buffers — it owns no socket,
// so it composes with the completion-style reactor and psx::os::Socket. The
// caller shuttles ciphertext between the socket and the engine:
//   feedEncrypted()  <- ciphertext read from the socket
//   takeEncrypted()  -> ciphertext to write to the socket
//   handshake()      advance the handshake (mutual auth against the CA)
//   write()/read()   application plaintext
class Tls {
public:
    static Result<Tls> create(const TlsConfig& config);

    Tls(Tls&&) noexcept;
    Tls& operator=(Tls&&) noexcept;
    Tls(const Tls&) = delete;
    Tls& operator=(const Tls&) = delete;
    ~Tls();

    // Hand the engine ciphertext received from the peer.
    void feedEncrypted(std::span<const char> ciphertext);
    // Pull ciphertext the engine wants sent to the peer ("" when none).
    std::string takeEncrypted();

    // Advance the handshake: Ok(true) established; Ok(false) needs more
    // ciphertext (feed, drain, retry); an error is fatal.
    Result<bool> handshake();
    bool established() const noexcept;

    // Encrypt plaintext (drain the result via takeEncrypted()).
    Result<void> write(std::span<const char> plaintext);
    // Decrypt any available application data ("" when none is ready).
    Result<std::string> read();

    // The peer certificate's first SAN URI — the node identity ("" if none).
    std::string peerSanUri() const;

private:
    Tls();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace psx::os
