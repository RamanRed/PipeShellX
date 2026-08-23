#include "psx/os/tls.hpp"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <string>

namespace psx::os {

namespace {

// op must be a string literal (Error stores it as a const char*). The OpenSSL
// error code goes in Error::code; details are on the OpenSSL error queue.
Error sslError(const char* op) {
    return Error{ErrorClass::Other, static_cast<int>(ERR_get_error()), op};
}

// Loads a PEM cert / key / CA held in memory into the SSL_CTX.
bool useCertificatePem(SSL_CTX* ctx, const std::string& pem) {
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (bio == nullptr) {
        return false;
    }
    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (cert == nullptr) {
        return false;
    }
    const bool ok = SSL_CTX_use_certificate(ctx, cert) == 1;
    X509_free(cert);
    return ok;
}

bool usePrivateKeyPem(SSL_CTX* ctx, const std::string& pem) {
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (bio == nullptr) {
        return false;
    }
    EVP_PKEY* key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (key == nullptr) {
        return false;
    }
    const bool ok = SSL_CTX_use_PrivateKey(ctx, key) == 1 && SSL_CTX_check_private_key(ctx) == 1;
    EVP_PKEY_free(key);
    return ok;
}

bool trustCaPem(SSL_CTX* ctx, const std::string& pem) {
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (bio == nullptr) {
        return false;
    }
    X509_STORE* store = SSL_CTX_get_cert_store(ctx);
    bool any = false;
    while (X509* ca = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr)) {
        any = X509_STORE_add_cert(store, ca) == 1 || any;
        X509_free(ca);
    }
    BIO_free(bio);
    return any;
}

} // namespace

struct Tls::Impl {
    SSL_CTX* ctx = nullptr;
    SSL* ssl = nullptr; // owns rbio/wbio after SSL_set_bio
    BIO* rbio = nullptr;
    BIO* wbio = nullptr;

    ~Impl() {
        if (ssl != nullptr) {
            SSL_free(ssl); // frees rbio + wbio
        }
        if (ctx != nullptr) {
            SSL_CTX_free(ctx);
        }
    }
};

Tls::Tls() : impl_(std::make_unique<Impl>()) {}
Tls::Tls(Tls&&) noexcept = default;
Tls& Tls::operator=(Tls&&) noexcept = default;
Tls::~Tls() = default;

Result<Tls> Tls::create(const TlsConfig& config) {
    Tls tls;
    Impl& impl = *tls.impl_;

    impl.ctx = SSL_CTX_new(config.isServer ? TLS_server_method() : TLS_client_method());
    if (impl.ctx == nullptr) {
        return sslError("SSL_CTX_new");
    }
    // Pin to TLS 1.3 and fail loudly if the build cannot honour it, rather than
    // silently negotiating a weaker version.
    if (SSL_CTX_set_min_proto_version(impl.ctx, TLS1_3_VERSION) != 1 ||
        SSL_CTX_set_max_proto_version(impl.ctx, TLS1_3_VERSION) != 1) {
        return sslError("pin TLS 1.3");
    }

    if (!useCertificatePem(impl.ctx, config.certificatePem)) {
        return sslError("load certificate");
    }
    if (!usePrivateKeyPem(impl.ctx, config.privateKeyPem)) {
        return sslError("load private key");
    }
    if (!trustCaPem(impl.ctx, config.caPem)) {
        return sslError("load CA");
    }
    // Mutual auth: both ends require and verify a peer certificate against the CA.
    SSL_CTX_set_verify(impl.ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);

    impl.ssl = SSL_new(impl.ctx);
    if (impl.ssl == nullptr) {
        return sslError("SSL_new");
    }
    impl.rbio = BIO_new(BIO_s_mem());
    impl.wbio = BIO_new(BIO_s_mem());
    if (impl.rbio == nullptr || impl.wbio == nullptr) {
        BIO_free(impl.rbio); // no-op on nullptr; frees the one that did succeed
        BIO_free(impl.wbio);
        impl.rbio = impl.wbio = nullptr;
        return sslError("BIO_new");
    }
    SSL_set_bio(impl.ssl, impl.rbio, impl.wbio); // SSL now owns both BIOs
    if (config.isServer) {
        SSL_set_accept_state(impl.ssl);
    } else {
        SSL_set_connect_state(impl.ssl);
    }
    return tls;
}

void Tls::feedEncrypted(std::span<const char> ciphertext) {
    if (!ciphertext.empty()) {
        BIO_write(impl_->rbio, ciphertext.data(), static_cast<int>(ciphertext.size()));
    }
}

std::string Tls::takeEncrypted() {
    std::string out;
    char buffer[4096];
    int n = 0;
    while ((n = BIO_read(impl_->wbio, buffer, sizeof(buffer))) > 0) {
        out.append(buffer, static_cast<std::size_t>(n));
    }
    return out;
}

Result<bool> Tls::handshake() {
    const int rc = SSL_do_handshake(impl_->ssl);
    if (rc == 1) {
        return true;
    }
    const int err = SSL_get_error(impl_->ssl, rc);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        return false; // needs more ciphertext
    }
    return sslError("SSL_do_handshake");
}

bool Tls::established() const noexcept {
    return SSL_is_init_finished(impl_->ssl) != 0;
}

Result<void> Tls::write(std::span<const char> plaintext) {
    if (plaintext.empty()) {
        return {};
    }
    const int rc = SSL_write(impl_->ssl, plaintext.data(), static_cast<int>(plaintext.size()));
    if (rc <= 0) {
        return sslError("SSL_write");
    }
    return {};
}

Result<std::string> Tls::read() {
    std::string out;
    char buffer[4096];
    while (true) {
        const int rc = SSL_read(impl_->ssl, buffer, sizeof(buffer));
        if (rc > 0) {
            out.append(buffer, static_cast<std::size_t>(rc));
            continue;
        }
        const int err = SSL_get_error(impl_->ssl, rc);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_ZERO_RETURN) {
            break; // no more application data available right now
        }
        return sslError("SSL_read");
    }
    return out;
}

std::string Tls::peerSanUri() const {
    X509* cert = SSL_get1_peer_certificate(impl_->ssl);
    if (cert == nullptr) {
        return {};
    }
    std::string uri;
    auto* names = static_cast<GENERAL_NAMES*>(X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr));
    if (names != nullptr) {
        for (int i = 0; i < sk_GENERAL_NAME_num(names) && uri.empty(); ++i) {
            const GENERAL_NAME* name = sk_GENERAL_NAME_value(names, i);
            if (name->type == GEN_URI) {
                const ASN1_IA5STRING* value = name->d.uniformResourceIdentifier;
                uri.assign(reinterpret_cast<const char*>(ASN1_STRING_get0_data(value)),
                           static_cast<std::size_t>(ASN1_STRING_length(value)));
            }
        }
        GENERAL_NAMES_free(names);
    }
    X509_free(cert);
    return uri;
}

} // namespace psx::os
