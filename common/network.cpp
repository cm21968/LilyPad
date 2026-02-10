#include "network.h"

namespace lilypad {

// ── WinsockInit ──

WinsockInit::WinsockInit() {
    WSADATA wsa;
    int result = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (result != 0) {
        throw std::runtime_error("WSAStartup failed with error: " + std::to_string(result));
    }
}

WinsockInit::~WinsockInit() {
    WSACleanup();
}

// ── Socket ──

Socket::Socket() : sock_(INVALID_SOCKET) {}

Socket::Socket(SOCKET s) : sock_(s) {}

Socket::~Socket() {
    close();
}

Socket::Socket(Socket&& other) noexcept : sock_(other.sock_) {
    other.sock_ = INVALID_SOCKET;
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        sock_ = other.sock_;
        other.sock_ = INVALID_SOCKET;
    }
    return *this;
}

void Socket::close() {
    if (sock_ != INVALID_SOCKET) {
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
}

bool Socket::send_all(const uint8_t* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int result = ::send(sock_, reinterpret_cast<const char*>(data + sent),
                            static_cast<int>(len - sent), 0);
        if (result == SOCKET_ERROR) {
            return false;
        }
        sent += static_cast<size_t>(result);
    }
    return true;
}

bool Socket::send_all(const std::vector<uint8_t>& data) {
    return send_all(data.data(), data.size());
}

bool Socket::recv_all(uint8_t* buf, size_t len) {
    size_t received = 0;
    while (received < len) {
        int result = ::recv(sock_, reinterpret_cast<char*>(buf + received),
                            static_cast<int>(len - received), 0);
        if (result <= 0) {
            return false; // error or connection closed
        }
        received += static_cast<size_t>(result);
    }
    return true;
}

// ── Factory helpers ──

Socket create_tcp_socket() {
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        throw std::runtime_error("Failed to create TCP socket: " + std::to_string(WSAGetLastError()));
    }
    return Socket(s);
}

Socket create_udp_socket() {
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        throw std::runtime_error("Failed to create UDP socket: " + std::to_string(WSAGetLastError()));
    }
    return Socket(s);
}

void set_nonblocking(SOCKET s) {
    u_long mode = 1;
    if (ioctlsocket(s, FIONBIO, &mode) != 0) {
        throw std::runtime_error("Failed to set non-blocking: " + std::to_string(WSAGetLastError()));
    }
}

} // namespace lilypad
