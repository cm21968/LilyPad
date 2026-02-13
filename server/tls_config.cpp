#include "tls_config.h"

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>

#include <fstream>
#include <iostream>

namespace lilypad {

static bool file_exists(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

static bool generate_self_signed(const std::string& cert_path, const std::string& key_path) {
    std::cout << "[TLS] Generating self-signed certificate...\n";

    EVP_PKEY* pkey = EVP_PKEY_new();
    if (!pkey) return false;

    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (!pctx) { EVP_PKEY_free(pkey); return false; }

    if (EVP_PKEY_keygen_init(pctx) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(pctx, 2048) <= 0 ||
        EVP_PKEY_keygen(pctx, &pkey) <= 0) {
        EVP_PKEY_CTX_free(pctx);
        EVP_PKEY_free(pkey);
        return false;
    }
    EVP_PKEY_CTX_free(pctx);

    X509* x509 = X509_new();
    if (!x509) { EVP_PKEY_free(pkey); return false; }

    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_get_notBefore(x509), 0);
    X509_gmtime_adj(X509_get_notAfter(x509), 365L * 24 * 60 * 60); // 1 year

    X509_set_pubkey(x509, pkey);

    X509_NAME* name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                reinterpret_cast<const unsigned char*>("LilyPad Server"), -1, -1, 0);
    X509_set_issuer_name(x509, name);

    X509_sign(x509, pkey, EVP_sha256());

    // Write cert
    FILE* cert_file = fopen(cert_path.c_str(), "wb");
    if (!cert_file) { X509_free(x509); EVP_PKEY_free(pkey); return false; }
    PEM_write_X509(cert_file, x509);
    fclose(cert_file);

    // Write key
    FILE* key_file = fopen(key_path.c_str(), "wb");
    if (!key_file) { X509_free(x509); EVP_PKEY_free(pkey); return false; }
    PEM_write_PrivateKey(key_file, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    fclose(key_file);

    X509_free(x509);
    EVP_PKEY_free(pkey);

    std::cout << "[TLS] Self-signed cert saved to " << cert_path << " and " << key_path << "\n";
    return true;
}

bool load_or_generate_cert(const std::string& cert_path, const std::string& key_path) {
    if (file_exists(cert_path) && file_exists(key_path)) {
        std::cout << "[TLS] Using existing cert: " << cert_path << "\n";
        return true;
    }
    return generate_self_signed(cert_path, key_path);
}

SSL_CTX* create_server_ssl_ctx(const std::string& cert_path, const std::string& key_path) {
    const SSL_METHOD* method = TLS_server_method();
    SSL_CTX* ctx = SSL_CTX_new(method);
    if (!ctx) {
        std::cerr << "[TLS] Failed to create SSL_CTX\n";
        return nullptr;
    }

    // Require TLS 1.2 minimum
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    if (SSL_CTX_use_certificate_chain_file(ctx, cert_path.c_str()) <= 0) {
        std::cerr << "[TLS] Failed to load certificate: " << cert_path << "\n";
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(ctx);
        return nullptr;
    }

    if (SSL_CTX_use_PrivateKey_file(ctx, key_path.c_str(), SSL_FILETYPE_PEM) <= 0) {
        std::cerr << "[TLS] Failed to load private key: " << key_path << "\n";
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(ctx);
        return nullptr;
    }

    if (!SSL_CTX_check_private_key(ctx)) {
        std::cerr << "[TLS] Private key does not match certificate\n";
        SSL_CTX_free(ctx);
        return nullptr;
    }

    std::cout << "[TLS] Server SSL context created successfully\n";
    return ctx;
}

} // namespace lilypad
