#include "tls_socket.h"

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <wincrypt.h>

#include <stdexcept>

namespace lilypad {

// ── OpenSSLInit ──

OpenSSLInit::OpenSSLInit() {
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
}

// ── TlsSocket ──

TlsSocket::TlsSocket() = default;

TlsSocket::~TlsSocket() {
    close();
}

TlsSocket::TlsSocket(TlsSocket&& other) noexcept
    : socket_(std::move(other.socket_)), ssl_(other.ssl_) {
    other.ssl_ = nullptr;
}

TlsSocket& TlsSocket::operator=(TlsSocket&& other) noexcept {
    if (this != &other) {
        close();
        socket_ = std::move(other.socket_);
        ssl_ = other.ssl_;
        other.ssl_ = nullptr;
    }
    return *this;
}

bool TlsSocket::accept(Socket&& raw_socket, SSL_CTX* ctx) {
    socket_ = std::move(raw_socket);

    ssl_ = SSL_new(ctx);
    if (!ssl_) return false;

    SSL_set_fd(ssl_, static_cast<int>(socket_.get()));

    int ret = SSL_accept(ssl_);
    if (ret <= 0) {
        SSL_free(ssl_);
        ssl_ = nullptr;
        return false;
    }

    return true;
}

bool TlsSocket::connect(Socket&& raw_socket, SSL_CTX* ctx) {
    socket_ = std::move(raw_socket);

    ssl_ = SSL_new(ctx);
    if (!ssl_) return false;

    SSL_set_fd(ssl_, static_cast<int>(socket_.get()));

    int ret = SSL_connect(ssl_);
    if (ret <= 0) {
        SSL_free(ssl_);
        ssl_ = nullptr;
        return false;
    }

    return true;
}

SOCKET TlsSocket::get() const {
    return socket_.get();
}

bool TlsSocket::valid() const {
    return socket_.valid() && ssl_ != nullptr;
}

void TlsSocket::close() {
    if (ssl_) {
        SSL_shutdown(ssl_);
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
    socket_.close();
}

bool TlsSocket::send_all(const uint8_t* data, size_t len) {
    if (!ssl_) return false;
    size_t sent = 0;
    while (sent < len) {
        int result = SSL_write(ssl_, data + sent, static_cast<int>(len - sent));
        if (result <= 0) {
            return false;
        }
        sent += static_cast<size_t>(result);
    }
    return true;
}

bool TlsSocket::send_all(const std::vector<uint8_t>& data) {
    return send_all(data.data(), data.size());
}

bool TlsSocket::recv_all(uint8_t* buf, size_t len) {
    if (!ssl_) return false;
    size_t received = 0;
    while (received < len) {
        int result = SSL_read(ssl_, buf + received, static_cast<int>(len - received));
        if (result <= 0) {
            return false;
        }
        received += static_cast<size_t>(result);
    }
    return true;
}

std::string TlsSocket::peer_ip() const {
    if (!socket_.valid()) return "";
    sockaddr_in addr{};
    int len = sizeof(addr);
    if (getpeername(socket_.get(), reinterpret_cast<sockaddr*>(&addr), &len) != 0)
        return "";
    char ip_str[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &addr.sin_addr, ip_str, sizeof(ip_str));
    return std::string(ip_str);
}

// ── Client SSL context factory ──

// Load CA certificates from the Windows system certificate store into OpenSSL
static bool load_windows_cert_store(SSL_CTX* ctx) {
    HCERTSTORE store = CertOpenSystemStoreA(0, "ROOT");
    if (!store) return false;

    X509_STORE* x509_store = SSL_CTX_get_cert_store(ctx);
    PCCERT_CONTEXT cert_ctx = nullptr;
    int loaded = 0;

    while ((cert_ctx = CertEnumCertificatesInStore(store, cert_ctx)) != nullptr) {
        const unsigned char* data = cert_ctx->pbCertEncoded;
        X509* x509 = d2i_X509(nullptr, &data, cert_ctx->cbCertEncoded);
        if (x509) {
            X509_STORE_add_cert(x509_store, x509);
            X509_free(x509);
            loaded++;
        }
    }

    CertCloseStore(store, 0);
    return loaded > 0;
}

SSL_CTX* create_client_ssl_ctx(bool trust_self_signed) {
    const SSL_METHOD* method = TLS_client_method();
    SSL_CTX* ctx = SSL_CTX_new(method);
    if (!ctx) return nullptr;

    // Require TLS 1.2 minimum
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    if (trust_self_signed) {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
    } else {
        // Load CA certs from the Windows certificate store
        load_windows_cert_store(ctx);
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
    }

    return ctx;
}

} // namespace lilypad
