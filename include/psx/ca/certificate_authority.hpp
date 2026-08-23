#pragma once

#include "psx/result.hpp"

#include <memory>
#include <string>
#include <vector>

namespace psx::ca {

// A leaf identity: a private key and its certificate (signed by the CA).
struct Identity {
    std::string privateKeyPem;
    std::string certificatePem;
};

// An offline fleet Certificate Authority: a self-signed root that issues leaf
// identities (node/controller certs) carrying a SAN-URI. The transport trusts
// the CA certificate and authorises peers by SAN URI (see NativeTransport). The
// header is std-only (pimpl): OpenSSL is confined to src/ca/.
class CertificateAuthority {
public:
    // Mint a brand-new CA (a self-signed root) named `commonName`.
    static Result<CertificateAuthority> create(const std::string& commonName);
    // Reload an existing CA from its PEM private key + certificate.
    static Result<CertificateAuthority> load(const std::string& caKeyPem, const std::string& caCertPem);

    CertificateAuthority(CertificateAuthority&&) noexcept;
    CertificateAuthority& operator=(CertificateAuthority&&) noexcept;
    CertificateAuthority(const CertificateAuthority&) = delete;
    CertificateAuthority& operator=(const CertificateAuthority&) = delete;
    ~CertificateAuthority();

    // Issue a leaf identity (fresh key + a certificate signed by this CA) whose
    // sole SAN URI is `sanUri` — the identity the peer authorises against.
    Result<Identity> issue(const std::string& sanUri) const;

    // The uppercase-hex serial number of a leaf certificate (the value a CRL
    // revokes it by). Static: reads the certificate, no CA state needed.
    static Result<std::string> serialHex(const std::string& certificatePem);

    // Issue a CRL (signed by this CA) revoking the given certificate serials
    // (uppercase hex). An empty list yields a valid, empty CRL. The CA cert
    // carries cRLSign, so peers that trust the CA accept this CRL.
    Result<std::string> issueCrl(const std::vector<std::string>& revokedSerialsHex) const;

    // The CA's own certificate — the trust anchor to distribute to every peer.
    std::string certificatePem() const;
    // The CA's private key — keep this offline; needed only to reload the CA.
    std::string privateKeyPem() const;

private:
    CertificateAuthority();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace psx::ca
