#include "psx/transport/tls_stream.hpp"

#include "psx/os/io.hpp"

#include <array>
#include <utility>

namespace psx::transport {

using psx::os::Interest;
using psx::os::Readiness;

TlsStream::TlsStream(psx::runtime::Reactor& reactor, psx::os::Socket socket, psx::os::Tls tls, Callbacks callbacks)
    : reactor_(reactor), socket_(std::move(socket)), tls_(std::move(tls)), callbacks_(std::move(callbacks)) {}

TlsStream::~TlsStream() {
    if (token_ != 0) {
        (void)reactor_.unwatch(token_);
    }
}

psx::Result<void> TlsStream::start() {
    auto watched =
        reactor_.watch(socket_.handle(), Interest::Readable, [this](Readiness readiness) { onEvent(readiness); });
    if (!watched.ok()) {
        return watched.error();
    }
    token_ = watched.value();
    // The client produces the ClientHello here; the server waits for it.
    driveHandshake();
    flushOutbound();
    updateInterest();
    return {};
}

void TlsStream::onEvent(Readiness readiness) {
    if (failed_) {
        return;
    }
    if (psx::os::has(readiness, Readiness::Writable)) {
        flushOutbound();
    }
    if (!failed_ && (psx::os::has(readiness, Readiness::Readable) || psx::os::has(readiness, Readiness::Hangup))) {
        onReadable();
    }
    if (!failed_ && psx::os::has(readiness, Readiness::Error)) {
        fail(psx::Error{psx::ErrorClass::Other, 0, "socket error"});
    }
}

void TlsStream::onReadable() {
    std::array<char, 16 * 1024> buffer{};
    while (true) { // edge-triggered: drain to WouldBlock
        auto got = psx::os::read(socket_.handle(), std::span<char>(buffer.data(), buffer.size()));
        if (got.ok()) {
            if (got.value() == 0) {
                fail(psx::Error{psx::ErrorClass::Other, 0, "peer closed the connection"});
                return;
            }
            tls_.feedEncrypted(std::span<const char>(buffer.data(), got.value()));
            continue;
        }
        if (got.error().cls == psx::ErrorClass::WouldBlock) {
            break;
        }
        fail(got.error());
        return;
    }

    if (!established_) {
        driveHandshake();
    } else {
        deliverAppData();
    }
    flushOutbound();
    updateInterest();
}

void TlsStream::driveHandshake() {
    if (established_ || failed_) {
        return;
    }
    auto result = tls_.handshake();
    if (!result.ok()) {
        fail(result.error());
        return;
    }
    outbound_ += tls_.takeEncrypted(); // ClientHello / ServerHello / Finished ...
    if (result.value()) {
        established_ = true;
        if (callbacks_.onReady) {
            callbacks_.onReady();
        }
        deliverAppData(); // application data may have ridden the final flight
    }
}

void TlsStream::deliverAppData() {
    if (failed_) {
        return;
    }
    auto data = tls_.read();
    if (!data.ok()) {
        fail(data.error());
        return;
    }
    if (!data.value().empty() && callbacks_.onData) {
        callbacks_.onData(data.value());
    }
}

void TlsStream::send(std::span<const char> plaintext) {
    if (failed_ || plaintext.empty()) {
        return;
    }
    if (auto wrote = tls_.write(plaintext); !wrote.ok()) {
        fail(wrote.error());
        return;
    }
    outbound_ += tls_.takeEncrypted();
    flushOutbound();
    updateInterest();
}

void TlsStream::flushOutbound() {
    while (!outbound_.empty()) {
        auto wrote = psx::os::write(socket_.handle(), std::span<const char>(outbound_.data(), outbound_.size()));
        if (wrote.ok()) {
            outbound_.erase(0, wrote.value());
            continue;
        }
        if (wrote.error().cls == psx::ErrorClass::WouldBlock) {
            break; // socket full; a Writable event resumes the flush
        }
        fail(wrote.error());
        return;
    }
}

void TlsStream::updateInterest() {
    if (failed_ || token_ == 0) {
        return;
    }
    const Interest interest = outbound_.empty() ? Interest::Readable : (Interest::Readable | Interest::Writable);
    (void)reactor_.modify(token_, interest);
}

void TlsStream::fail(const psx::Error& error) {
    if (failed_) {
        return;
    }
    failed_ = true;
    if (token_ != 0) {
        (void)reactor_.unwatch(token_);
        token_ = 0;
    }
    if (callbacks_.onError) {
        callbacks_.onError(error);
    }
}

} // namespace psx::transport
