#include "psx/ca/certificate_authority.hpp"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <string>

namespace psx::ca {

namespace {

// op must be a string literal (Error stores it as a const char*).
Error caError(const char* op) {
    return Error{ErrorClass::Other, static_cast<int>(ERR_get_error()), op};
}

std::string bioToString(BIO* bio) {
    char* data = nullptr;
    const long len = BIO_get_mem_data(bio, &data);
    return std::string(data, static_cast<std::size_t>(len));
}

std::string keyToPem(EVP_PKEY* key) {
    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr);
    std::string pem = bioToString(bio);
    BIO_free(bio);
    return pem;
}

std::string certToPem(X509* cert) {
    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(bio, cert);
    std::string pem = bioToString(bio);
    BIO_free(bio);
    return pem;
}

EVP_PKEY* keyFromPem(const std::string& pem) {
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    EVP_PKEY* key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    return key;
}

X509* certFromPem(const std::string& pem) {
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    return cert;
}

bool setSerialRandom(X509* cert) {
    unsigned char bytes[16];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1) {
        return false;
    }
    bytes[0] &= 0x7F; // positive
    BIGNUM* bn = BN_bin2bn(bytes, sizeof(bytes), nullptr);
    const bool ok = bn != nullptr && BN_to_ASN1_INTEGER(bn, X509_get_serialNumber(cert)) != nullptr;
    BN_free(bn);
    return ok;
}

bool addExtension(X509* cert, X509* issuer, int nid, const char* value) {
    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, issuer, cert, nullptr, nullptr, 0);
    X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value);
    if (ext == nullptr) {
        return false;
    }
    const bool ok = X509_add_ext(cert, ext, -1) == 1;
    X509_EXTENSION_free(ext);
    return ok;
}

void setName(X509_NAME* name, const std::string& cn) {
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>(cn.c_str()), -1, -1, 0);
}

} // namespace

struct CertificateAuthority::Impl {
    EVP_PKEY* key = nullptr;
    X509* cert = nullptr;
    ~Impl() {
        if (key != nullptr) {
            EVP_PKEY_free(key);
        }
        if (cert != nullptr) {
            X509_free(cert);
        }
    }
};

CertificateAuthority::CertificateAuthority() : impl_(std::make_unique<Impl>()) {}
CertificateAuthority::CertificateAuthority(CertificateAuthority&&) noexcept = default;
CertificateAuthority& CertificateAuthority::operator=(CertificateAuthority&&) noexcept = default;
CertificateAuthority::~CertificateAuthority() = default;

Result<CertificateAuthority> CertificateAuthority::create(const std::string& commonName) {
    CertificateAuthority ca;
    Impl& impl = *ca.impl_;

    impl.key = EVP_RSA_gen(3072);
    if (impl.key == nullptr) {
        return caError("generate CA key");
    }
    impl.cert = X509_new();
    if (impl.cert == nullptr) {
        return caError("X509_new");
    }
    X509_set_version(impl.cert, 2); // v3
    if (!setSerialRandom(impl.cert)) {
        return caError("serial");
    }
    X509_gmtime_adj(X509_getm_notBefore(impl.cert), 0);
    X509_gmtime_adj(X509_getm_notAfter(impl.cert), 3650L * 24 * 3600); // 10 years
    X509_set_pubkey(impl.cert, impl.key);
    setName(X509_get_subject_name(impl.cert), commonName);
    X509_set_issuer_name(impl.cert, X509_get_subject_name(impl.cert)); // self-signed
    if (!addExtension(impl.cert, impl.cert, NID_basic_constraints, "critical,CA:TRUE") ||
        !addExtension(impl.cert, impl.cert, NID_key_usage, "critical,keyCertSign,cRLSign") ||
        !addExtension(impl.cert, impl.cert, NID_subject_key_identifier, "hash")) {
        return caError("CA extensions");
    }
    if (X509_sign(impl.cert, impl.key, EVP_sha256()) == 0) {
        return caError("sign CA cert");
    }
    return ca;
}

Result<CertificateAuthority> CertificateAuthority::load(const std::string& caKeyPem, const std::string& caCertPem) {
    CertificateAuthority ca;
    ca.impl_->key = keyFromPem(caKeyPem);
    ca.impl_->cert = certFromPem(caCertPem);
    if (ca.impl_->key == nullptr || ca.impl_->cert == nullptr) {
        return caError("load CA PEM");
    }
    if (X509_check_private_key(ca.impl_->cert, ca.impl_->key) != 1) {
        return caError("CA key does not match cert");
    }
    return ca;
}

Result<Identity> CertificateAuthority::issue(const std::string& sanUri) const {
    EVP_PKEY* leafKey = EVP_RSA_gen(2048);
    if (leafKey == nullptr) {
        return caError("generate leaf key");
    }
    X509* leaf = X509_new();
    if (leaf == nullptr) {
        EVP_PKEY_free(leafKey);
        return caError("X509_new");
    }

    Result<Identity> result = Error{ErrorClass::Other, 0, "issue"};
    do {
        X509_set_version(leaf, 2);
        if (!setSerialRandom(leaf)) {
            break;
        }
        X509_gmtime_adj(X509_getm_notBefore(leaf), 0);
        X509_gmtime_adj(X509_getm_notAfter(leaf), 365L * 24 * 3600); // 1 year
        X509_set_pubkey(leaf, leafKey);
        setName(X509_get_subject_name(leaf), sanUri);
        X509_set_issuer_name(leaf, X509_get_subject_name(impl_->cert)); // signed by the CA
        const std::string san = "URI:" + sanUri;
        if (!addExtension(leaf, impl_->cert, NID_basic_constraints, "critical,CA:FALSE") ||
            !addExtension(leaf, impl_->cert, NID_key_usage, "critical,digitalSignature,keyEncipherment") ||
            !addExtension(leaf, impl_->cert, NID_ext_key_usage, "serverAuth,clientAuth") ||
            !addExtension(leaf, impl_->cert, NID_subject_alt_name, san.c_str())) {
            break;
        }
        if (X509_sign(leaf, impl_->key, EVP_sha256()) == 0) {
            break;
        }
        result = Identity{.privateKeyPem = keyToPem(leafKey), .certificatePem = certToPem(leaf)};
    } while (false);

    X509_free(leaf);
    EVP_PKEY_free(leafKey);
    return result;
}

std::string CertificateAuthority::certificatePem() const {
    return certToPem(impl_->cert);
}
std::string CertificateAuthority::privateKeyPem() const {
    return keyToPem(impl_->key);
}

} // namespace psx::ca
