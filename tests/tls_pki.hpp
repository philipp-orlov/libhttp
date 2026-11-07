// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

// A throwaway certificate authority for the TLS tests: a self-signed certificate and key in temp files.

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <memory>

namespace testsupport {

inline void pkiExpect(bool condition, const char* what)
{
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        std::_Exit(1);
    }
}

// A self-signed certificate for localhost and 127.0.0.1, written to two temp files.
struct Pki
{
    char certificate[32] = "/tmp/libhttp-wss-cert-XXXXXX";
    char key[32] = "/tmp/libhttp-wss-key-XXXXXX";
    char otherCertificate[32] = "/tmp/libhttp-wss-oth-XXXXXX";
    ~Pki()
    {
        unlink(certificate);
        unlink(key);
        unlink(otherCertificate);
    }

    static void write(const char* certPath, const char* keyPath, const char* commonName, const char* alternativeNames)
    {
        std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> generator(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
        pkiExpect(generator && EVP_PKEY_keygen_init(generator.get()) == 1 && EVP_PKEY_CTX_set_rsa_keygen_bits(generator.get(), 2048) == 1, "keygen setup");
        EVP_PKEY* generated = nullptr;
        pkiExpect(EVP_PKEY_keygen(generator.get(), &generated) == 1, "keygen");
        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> pkey(generated, EVP_PKEY_free);
        std::unique_ptr<X509, decltype(&X509_free)> cert(X509_new(), X509_free);
        X509_set_version(cert.get(), 2);
        ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1);
        X509_gmtime_adj(X509_getm_notBefore(cert.get()), -60);
        X509_gmtime_adj(X509_getm_notAfter(cert.get()), 3600);
        X509_set_pubkey(cert.get(), pkey.get());
        auto* name = X509_get_subject_name(cert.get());
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>(commonName), -1, -1, 0);
        X509_set_issuer_name(cert.get(), name);
        X509V3_CTX ctx;
        X509V3_set_ctx_nodb(&ctx);
        X509V3_set_ctx(&ctx, cert.get(), cert.get(), nullptr, nullptr, 0);
        std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> san(
            X509V3_EXT_conf_nid(nullptr, &ctx, NID_subject_alt_name, const_cast<char*>(alternativeNames)), X509_EXTENSION_free);
        pkiExpect(san != nullptr, "san");
        X509_add_ext(cert.get(), san.get(), -1);
        pkiExpect(X509_sign(cert.get(), pkey.get(), EVP_sha256()) > 0, "sign");
        FILE* c = fopen(certPath, "w");
        FILE* k = fopen(keyPath, "w");
        pkiExpect(c && k, "temp files");
        pkiExpect(PEM_write_X509(c, cert.get()) == 1 && PEM_write_PrivateKey(k, pkey.get(), nullptr, nullptr, 0, nullptr, nullptr) == 1, "pem");
        fclose(c);
        fclose(k);
    }
    Pki()
    {
        close(mkstemp(certificate));
        close(mkstemp(key));
        close(mkstemp(otherCertificate));
        write(certificate, key, "localhost", "DNS:localhost,IP:127.0.0.1");
        char unusedKey[32] = "/tmp/libhttp-wss-unk-XXXXXX";
        close(mkstemp(unusedKey));
        write(otherCertificate, unusedKey, "elsewhere", "DNS:elsewhere.example");
        unlink(unusedKey);
    }
};

}  // namespace testsupport
