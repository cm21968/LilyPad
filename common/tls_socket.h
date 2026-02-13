#pragma once

#include "network.h"

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>

#include <memory>
#include <string>
#include <vector>

namespace lilypad {

// RAII OpenSSL library initializer -- create one at program start
class OpenSSLInit {
public:
    OpenSSLInit();
    ~OpenSSLInit() = default;
    OpenSSLInit(const OpenSSLInit&) = delete;
    OpenSSLInit& operator=(const OpenSSLInit&) = delete;
};

// RAII TLS socket wrapper -- mirrors Socket API (send_all, recv_all, get, valid, close)
class TlsSocket {
public:
    TlsSocket();
    ~TlsSocket();

    TlsSocket(TlsSocket&& other) noexcept;
    TlsSocket& operator=(TlsSocket&& other) noexcept;
    TlsSocket(const TlsSocket&) = delete;
    TlsSocket& operator=(const TlsSocket&) = delete;

    // Server-side: wrap an accepted raw socket with TLS
    // Takes ownership of the Socket. Returns false if TLS handshake fails.
    bool accept(Socket&& raw_socket, SSL_CTX* ctx);

    // Client-side: wrap a connected raw socket with TLS
    // Takes ownership of the Socket. Returns false if TLS handshake fails.
    // If trust_self_signed is true, disables certificate verification.
    bool connect(Socket&& raw_socket, SSL_CTX* ctx);

    // Underlying socket handle (for select() calls)
    SOCKET get() const;
    bool   valid() const;
    void   close();

    // Send exactly `len` bytes (loops until complete). Returns false on error.
    bool send_all(const uint8_t* data, size_t len);
    bool send_all(const std::vector<uint8_t>& data);

    // Receive exactly `len` bytes into `buf`. Returns false on error/disconnect.
    bool recv_all(uint8_t* buf, size_t len);

    // Get the peer's IP address as a string
    std::string peer_ip() const;

private:
    Socket socket_;
    SSL*   ssl_ = nullptr;
};

// Create a client SSL_CTX with default verification settings
// If trust_self_signed is true, sets SSL_CTX_set_verify to SSL_VERIFY_NONE
SSL_CTX* create_client_ssl_ctx(bool trust_self_signed = false);

} // namespace lilypad
