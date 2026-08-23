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

std::string reqToPem(X509_REQ* req) {
    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509_REQ(bio, req);
    std::string pem = bioToString(bio);
    BIO_free(bio);
    return pem;
}

X509_REQ* reqFromPem(const std::string& pem) {
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    X509_REQ* req = PEM_read_bio_X509_REQ(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    return req;
}

std::string crlToPem(X509_CRL* crl) {
    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509_CRL(bio, crl);
    std::string pem = bioToString(bio);
    BIO_free(bio);
    return pem;
}

// Uppercase-hex serial of a certificate; empty on failure.
std::string serialHexOf(X509* cert) {
    const ASN1_INTEGER* serial = X509_get0_serialNumber(cert);
    BIGNUM* bn = ASN1_INTEGER_to_BN(serial, nullptr);
    if (bn == nullptr) {
        return {};
    }
    char* hex = BN_bn2hex(bn); // OpenSSL returns uppercase hex
    std::string out = hex != nullptr ? std::string(hex) : std::string();
    OPENSSL_free(hex);
    BN_free(bn);
    return out;
}

// Builds an ASN1_INTEGER from an uppercase/lowercase-hex serial; null on failure.
ASN1_INTEGER* serialFromHex(const std::string& hex) {
    BIGNUM* bn = nullptr;
    if (BN_hex2bn(&bn, hex.c_str()) == 0) {
        BN_free(bn);
        return nullptr;
    }
    ASN1_INTEGER* out = BN_to_ASN1_INTEGER(bn, nullptr);
    BN_free(bn);
    return out;
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

// Builds a leaf certificate carrying `leafPubKey`, SAN `sanUri`, signed by the
// CA (caCert/caKey). Returns the cert PEM, or empty on failure. Shared by
// issue() (fresh key) and signCsr() (a node's key, via its CSR).
std::string buildAndSignLeaf(X509* caCert, EVP_PKEY* caKey, EVP_PKEY* leafPubKey, const std::string& sanUri) {
    X509* leaf = X509_new();
    if (leaf == nullptr) {
        return {};
    }
    std::string pem;
    do {
        X509_set_version(leaf, 2); // v3
        if (!setSerialRandom(leaf)) {
            break;
        }
        X509_gmtime_adj(X509_getm_notBefore(leaf), 0);
        X509_gmtime_adj(X509_getm_notAfter(leaf), 365L * 24 * 3600); // 1 year
        X509_set_pubkey(leaf, leafPubKey);
        setName(X509_get_subject_name(leaf), sanUri);
        X509_set_issuer_name(leaf, X509_get_subject_name(caCert)); // signed by the CA
        const std::string san = "URI:" + sanUri;
        if (!addExtension(leaf, caCert, NID_basic_constraints, "critical,CA:FALSE") ||
            !addExtension(leaf, caCert, NID_key_usage, "critical,digitalSignature,keyEncipherment") ||
            !addExtension(leaf, caCert, NID_ext_key_usage, "serverAuth,clientAuth") ||
            !addExtension(leaf, caCert, NID_subject_alt_name, san.c_str())) {
            break;
        }
        if (X509_sign(leaf, caKey, EVP_sha256()) == 0) {
            break;
        }
        pem = certToPem(leaf);
    } while (false);
    X509_free(leaf);
    return pem;
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
    const std::string certPem = buildAndSignLeaf(impl_->cert, impl_->key, leafKey, sanUri);
    Result<Identity> result =
        certPem.empty() ? Result<Identity>(Error{ErrorClass::Other, 0, "issue"})
                        : Result<Identity>(Identity{.privateKeyPem = keyToPem(leafKey), .certificatePem = certPem});
    EVP_PKEY_free(leafKey);
    return result;
}

Result<KeyAndCsr> CertificateAuthority::generateCsr(const std::string& sanUri) {
    EVP_PKEY* key = EVP_RSA_gen(2048);
    if (key == nullptr) {
        return caError("generate key");
    }
    X509_REQ* req = X509_REQ_new();
    if (req == nullptr) {
        EVP_PKEY_free(key);
        return caError("X509_REQ_new");
    }
    Result<KeyAndCsr> result = Error{ErrorClass::Other, 0, "generate CSR"};
    do {
        X509_REQ_set_version(req, 0); // v1
        setName(X509_REQ_get_subject_name(req), sanUri);
        if (X509_REQ_set_pubkey(req, key) == 0 || X509_REQ_sign(req, key, EVP_sha256()) == 0) {
            break;
        }
        result = KeyAndCsr{.privateKeyPem = keyToPem(key), .csrPem = reqToPem(req)};
    } while (false);
    X509_REQ_free(req);
    EVP_PKEY_free(key);
    return result;
}

Result<std::string> CertificateAuthority::signCsr(const std::string& csrPem, const std::string& sanUri) const {
    X509_REQ* req = reqFromPem(csrPem);
    if (req == nullptr) {
        return caError("parse CSR");
    }
    EVP_PKEY* pub = X509_REQ_get_pubkey(req);
    Result<std::string> result = Error{ErrorClass::Other, 0, "sign CSR"};
    do {
        if (pub == nullptr) {
            result = caError("CSR public key");
            break;
        }
        if (X509_REQ_verify(req, pub) != 1) { // proof the requester holds the private key
            result = caError("CSR signature");
            break;
        }
        const std::string cert = buildAndSignLeaf(impl_->cert, impl_->key, pub, sanUri);
        if (!cert.empty()) {
            result = cert;
        }
    } while (false);
    EVP_PKEY_free(pub);
    X509_REQ_free(req);
    return result;
}

Result<std::string> CertificateAuthority::serialHex(const std::string& certificatePem) {
    X509* cert = certFromPem(certificatePem);
    if (cert == nullptr) {
        return caError("parse certificate");
    }
    std::string hex = serialHexOf(cert);
    X509_free(cert);
    if (hex.empty()) {
        return caError("read serial");
    }
    return hex;
}

Result<std::string> CertificateAuthority::issueCrl(const std::vector<std::string>& revokedSerialsHex) const {
    X509_CRL* crl = X509_CRL_new();
    if (crl == nullptr) {
        return caError("X509_CRL_new");
    }

    Result<std::string> result = Error{ErrorClass::Other, 0, "issue CRL"};
    ASN1_TIME* now = ASN1_TIME_new();
    ASN1_TIME* next = ASN1_TIME_new();
    do {
        if (now == nullptr || next == nullptr) {
            break;
        }
        X509_CRL_set_version(crl, 1); // v2
        X509_CRL_set_issuer_name(crl, X509_get_subject_name(impl_->cert));
        X509_gmtime_adj(now, 0);
        X509_gmtime_adj(next, 30L * 24 * 3600); // valid for 30 days
        if (X509_CRL_set1_lastUpdate(crl, now) == 0 || X509_CRL_set1_nextUpdate(crl, next) == 0) {
            break;
        }
        bool entriesOk = true;
        for (const std::string& hex : revokedSerialsHex) {
            X509_REVOKED* revoked = X509_REVOKED_new();
            ASN1_INTEGER* serial = serialFromHex(hex);
            if (revoked == nullptr || serial == nullptr || X509_REVOKED_set_serialNumber(revoked, serial) == 0 ||
                X509_REVOKED_set_revocationDate(revoked, now) == 0 || X509_CRL_add0_revoked(crl, revoked) == 0) {
                ASN1_INTEGER_free(serial);
                X509_REVOKED_free(revoked);
                entriesOk = false;
                break;
            }
            ASN1_INTEGER_free(serial); // set_serialNumber copies
        }
        if (!entriesOk) {
            break;
        }
        X509_CRL_sort(crl);
        if (X509_CRL_sign(crl, impl_->key, EVP_sha256()) == 0) {
            break;
        }
        result = crlToPem(crl);
    } while (false);

    ASN1_TIME_free(now);
    ASN1_TIME_free(next);
    X509_CRL_free(crl);
    return result;
}

std::string CertificateAuthority::certificatePem() const {
    return certToPem(impl_->cert);
}
std::string CertificateAuthority::privateKeyPem() const {
    return keyToPem(impl_->key);
}

} // namespace psx::ca
