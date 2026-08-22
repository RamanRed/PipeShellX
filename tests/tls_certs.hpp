#pragma once

// Test-only mTLS material: ephemeral self-signed certs generated via the OpenSSL
// C API (no fixtures on disk).

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <string>

namespace tls_test {

struct KeyCert {
    std::string keyPem;
    std::string certPem;
};

inline std::string bioToString(BIO* bio) {
    char* data = nullptr;
    const long len = BIO_get_mem_data(bio, &data);
    return std::string(data, static_cast<std::size_t>(len));
}

// A self-signed certificate (its own CA) carrying `sanUri` as a SAN URI.
inline KeyCert generateSelfSigned(const std::string& sanUri) {
    EVP_PKEY* pkey = EVP_RSA_gen(2048);
    X509* x = X509_new();
    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    X509_gmtime_adj(X509_getm_notBefore(x), 0);
    X509_gmtime_adj(X509_getm_notAfter(x), 31536000L);
    X509_set_pubkey(x, pkey);
    X509_NAME* name = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("psx-test"), -1, -1, 0);
    X509_set_issuer_name(x, name);
    const std::string san = "URI:" + sanUri;
    X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, nullptr, NID_subject_alt_name, san.c_str());
    X509_add_ext(x, ext, -1);
    X509_EXTENSION_free(ext);
    X509_sign(x, pkey, EVP_sha256());

    KeyCert kc;
    BIO* keyBio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(keyBio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    kc.keyPem = bioToString(keyBio);
    BIO_free(keyBio);
    BIO* certBio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(certBio, x);
    kc.certPem = bioToString(certBio);
    BIO_free(certBio);

    X509_free(x);
    EVP_PKEY_free(pkey);
    return kc;
}

} // namespace tls_test
