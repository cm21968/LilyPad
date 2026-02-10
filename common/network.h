#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace lilypad {

// ── RAII Winsock initializer — create one at program start ──
class WinsockInit {
public:
    WinsockInit();
    ~WinsockInit();
    WinsockInit(const WinsockInit&) = delete;
    WinsockInit& operator=(const WinsockInit&) = delete;
};

// ── RAII socket wrapper ──
class Socket {
public:
    Socket();                          // wraps INVALID_SOCKET
    explicit Socket(SOCKET s);         // take ownership
    ~Socket();

    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    SOCKET get() const { return sock_; }
    bool   valid() const { return sock_ != INVALID_SOCKET; }
    void   close();

    // Send exactly `len` bytes (loops until complete). Returns false on error.
    bool send_all(const uint8_t* data, size_t len);
    bool send_all(const std::vector<uint8_t>& data);

    // Receive exactly `len` bytes into `buf`. Returns false on error/disconnect.
    bool recv_all(uint8_t* buf, size_t len);

private:
    SOCKET sock_;
};

// ── Factory helpers ──
Socket create_tcp_socket();
Socket create_udp_socket();
void   set_nonblocking(SOCKET s);

} // namespace lilypad
