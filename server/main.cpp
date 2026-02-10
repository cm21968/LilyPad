#include "network.h"
#include "protocol.h"

#include <atomic>
#include <condition_variable>
#include <csignal>
#include <deque>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ── Configuration ──
constexpr uint16_t TCP_PORT = 7777;
constexpr uint16_t UDP_PORT = 7778;

// ── Global state ──
static std::atomic<bool> g_running{true};

void signal_handler(int) {
    g_running = false;
}

struct ClientInfo {
    uint32_t           id;
    std::string        username;
    lilypad::Socket    tcp_socket;
    sockaddr_in        udp_addr;    // filled in when first UDP packet arrives
    bool               udp_known = false;

    // Screen sharing
    bool                             screen_sharing = false;
    std::unordered_set<uint32_t>     screen_subscribers; // IDs of clients watching this user
};

static std::mutex                                   g_clients_mutex;
static std::unordered_map<uint32_t, ClientInfo>     g_clients;
static uint32_t                                     g_next_id = 1;

// ── Chat history (persists for the lifetime of the server) ──
struct ChatEntry {
    uint32_t    sender_id;
    std::string text;
};
static std::mutex                g_chat_mutex;
static std::vector<ChatEntry>    g_chat_history;

// ── Update notification (loaded from update.txt next to the server executable) ──
// File format: line 1 = version, line 2 = download URL
static std::string g_update_version;
static std::string g_update_url;

static void load_update_config() {
    std::ifstream file("update.txt");
    if (!file.is_open()) return;
    std::getline(file, g_update_version);
    std::getline(file, g_update_url);
    if (!g_update_version.empty() && !g_update_url.empty()) {
        std::cout << "[Server] Update configured: v" << g_update_version
                  << " at " << g_update_url << "\n";
    } else {
        g_update_version.clear();
        g_update_url.clear();
    }
}

// ── Screen relay queue (decouples tcp_read_loop from blocking subscriber sends) ──
struct RelayItem {
    std::vector<uint8_t> data;
    uint32_t             sharer_id;
    bool                 is_audio;  // true = SCREEN_AUDIO (high priority)
};

static std::mutex               g_relay_mutex;
static std::condition_variable  g_relay_cv;
static std::deque<RelayItem>    g_relay_queue;

static void enqueue_relay(std::vector<uint8_t> data, uint32_t sharer_id, bool is_audio) {
    {
        std::lock_guard<std::mutex> lock(g_relay_mutex);
        g_relay_queue.push_back({std::move(data), sharer_id, is_audio});
        // Limit queue depth — drop oldest non-audio items to prevent unbounded growth
        while (g_relay_queue.size() > 60) {
            // Find and remove the oldest video frame (keep audio)
            bool dropped = false;
            for (auto it = g_relay_queue.begin(); it != g_relay_queue.end(); ++it) {
                if (!it->is_audio) {
                    g_relay_queue.erase(it);
                    dropped = true;
                    break;
                }
            }
            if (!dropped) break; // all items are audio, stop dropping
        }
    }
    g_relay_cv.notify_one();
}

// ── Broadcast a TCP message to all clients (caller must hold g_clients_mutex) ──
static void broadcast_tcp(const std::vector<uint8_t>& msg) {
    for (auto& [id, client] : g_clients) {
        client.tcp_socket.send_all(msg);
    }
}

// ── Remove a client and notify others ──
static void remove_client(uint32_t client_id) {
    std::string name;
    {
        std::lock_guard<std::mutex> lock(g_clients_mutex);
        auto it = g_clients.find(client_id);
        if (it == g_clients.end()) return;
        name = it->second.username;

        // If this client was sharing, broadcast SCREEN_STOP
        if (it->second.screen_sharing) {
            auto stop_msg = lilypad::make_screen_stop_broadcast(client_id);
            for (auto& [id, c] : g_clients) {
                if (id != client_id)
                    c.tcp_socket.send_all(stop_msg);
            }
        }

        // Remove this client from all subscriber sets
        for (auto& [id, c] : g_clients) {
            c.screen_subscribers.erase(client_id);
        }

        it->second.tcp_socket.close();
        g_clients.erase(it);

        // Broadcast USER_LEFT to remaining clients
        auto msg = lilypad::make_user_left_msg(client_id);
        broadcast_tcp(msg);
    }
    std::cout << "[Server] " << name << " (id=" << client_id << ") left.\n";
}

// ── Thread 1: Accept new TCP connections ──
static void tcp_accept_loop(SOCKET listen_sock) {
    while (g_running) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(listen_sock, &read_set);

        timeval timeout{};
        timeout.tv_sec  = 0;
        timeout.tv_usec = 200000; // 200ms

        int ready = select(0, &read_set, nullptr, nullptr, &timeout);
        if (ready <= 0) continue;

        sockaddr_in client_addr{};
        int addr_len = sizeof(client_addr);
        SOCKET new_sock = accept(listen_sock,
                                 reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
        if (new_sock == INVALID_SOCKET) continue;

        // Disable Nagle's algorithm for low-latency sends
        int nodelay = 1;
        setsockopt(new_sock, IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

        // Large send buffer so relay sends copy to kernel buffer quickly
        // instead of blocking the relay thread
        int sndbuf = 1024 * 1024; // 1MB
        setsockopt(new_sock, SOL_SOCKET, SO_SNDBUF,
                   reinterpret_cast<const char*>(&sndbuf), sizeof(sndbuf));

        lilypad::Socket client_tcp(new_sock);

        // Read the signal header (expect JOIN)
        uint8_t hdr_buf[lilypad::SIGNAL_HEADER_SIZE];
        if (!client_tcp.recv_all(hdr_buf, lilypad::SIGNAL_HEADER_SIZE)) {
            continue; // failed to read header, drop connection
        }

        auto header = lilypad::deserialize_header(hdr_buf);
        if (header.type != lilypad::MsgType::JOIN || header.payload_len == 0 ||
            header.payload_len > lilypad::MAX_USERNAME_LEN + 1) {
            continue; // invalid first message
        }

        // Read the username payload
        std::vector<uint8_t> payload(header.payload_len);
        if (!client_tcp.recv_all(payload.data(), header.payload_len)) {
            continue;
        }
        std::string username(reinterpret_cast<const char*>(payload.data()));
        if (username.empty()) username = "Unknown";

        // Assign an ID
        uint32_t client_id;
        {
            std::lock_guard<std::mutex> lock(g_clients_mutex);
            client_id = g_next_id++;

            // Send WELCOME to the new client
            auto welcome = lilypad::make_welcome_msg(client_id, UDP_PORT);
            client_tcp.send_all(welcome);

            // Send update notification if configured
            if (!g_update_version.empty() && !g_update_url.empty()) {
                auto update_msg = lilypad::make_update_available_msg(
                    g_update_version, g_update_url);
                client_tcp.send_all(update_msg);
            }

            // Send existing user list to the new client
            for (auto& [id, existing] : g_clients) {
                auto msg = lilypad::make_user_joined_msg(existing.id, existing.username);
                client_tcp.send_all(msg);
            }

            // Send SCREEN_START for any currently-sharing users
            for (auto& [id, existing] : g_clients) {
                if (existing.screen_sharing) {
                    auto msg = lilypad::make_screen_start_broadcast(existing.id);
                    client_tcp.send_all(msg);
                }
            }

            // Send chat history to the new client
            {
                std::lock_guard<std::mutex> chat_lock(g_chat_mutex);
                for (auto& entry : g_chat_history) {
                    auto msg = lilypad::make_text_chat_broadcast_msg(entry.sender_id, entry.text);
                    client_tcp.send_all(msg);
                }
            }

            // Broadcast USER_JOINED to all existing clients
            auto joined_msg = lilypad::make_user_joined_msg(client_id, username);
            broadcast_tcp(joined_msg);

            // Add the new client
            ClientInfo info;
            info.id         = client_id;
            info.username   = username;
            info.tcp_socket = std::move(client_tcp);
            info.udp_known  = false;
            g_clients[client_id] = std::move(info);
        }

        std::cout << "[Server] " << username << " (id=" << client_id << ") joined.\n";
    }
}

// ── Non-blocking send helper for video frames ──
// Attempts to send the full buffer without blocking. Returns true if fully sent,
// false if the socket would block (frame should be skipped for this subscriber).
static bool try_send_nonblocking(SOCKET s, const uint8_t* data, size_t len) {
    // Set non-blocking
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);

    const char* ptr = reinterpret_cast<const char*>(data);
    int remaining = static_cast<int>(len);
    bool ok = true;

    while (remaining > 0) {
        int sent = send(s, ptr, remaining, 0);
        if (sent > 0) {
            ptr += sent;
            remaining -= sent;
        } else {
            // WSAEWOULDBLOCK or error — skip this frame for this subscriber
            ok = false;
            break;
        }
    }

    // Restore blocking mode
    mode = 0;
    ioctlsocket(s, FIONBIO, &mode);
    return ok;
}

// ── Thread 2: Dedicated screen relay thread ──
// Drains the relay queue and sends to subscribers. Audio items are sent first.
static void screen_relay_loop() {
    while (g_running) {
        std::vector<RelayItem> audio_items;
        std::vector<RelayItem> frame_items;

        {
            std::unique_lock<std::mutex> lock(g_relay_mutex);
            g_relay_cv.wait_for(lock, std::chrono::milliseconds(5), [] {
                return !g_relay_queue.empty() || !g_running;
            });
            if (!g_running && g_relay_queue.empty()) break;

            // Partition into audio (high priority) and frame (low priority)
            for (auto& item : g_relay_queue) {
                if (item.is_audio)
                    audio_items.push_back(std::move(item));
                else
                    frame_items.push_back(std::move(item));
            }
            g_relay_queue.clear();
        }

        // Helper: collect subscriber sockets for a relay item
        auto get_subscriber_sockets = [](uint32_t sharer_id) {
            std::vector<SOCKET> sub_sockets;
            std::lock_guard<std::mutex> lock(g_clients_mutex);
            auto it = g_clients.find(sharer_id);
            if (it != g_clients.end()) {
                for (uint32_t sub_id : it->second.screen_subscribers) {
                    auto sub_it = g_clients.find(sub_id);
                    if (sub_it != g_clients.end()) {
                        sub_sockets.push_back(sub_it->second.tcp_socket.get());
                    }
                }
            }
            return sub_sockets;
        };

        // Send audio first (high priority) — blocking sends (small packets)
        for (auto& item : audio_items) {
            auto subs = get_subscriber_sockets(item.sharer_id);
            for (SOCKET s : subs) {
                const char* ptr = reinterpret_cast<const char*>(item.data.data());
                int remaining = static_cast<int>(item.data.size());
                while (remaining > 0) {
                    int sent = send(s, ptr, remaining, 0);
                    if (sent <= 0) break;
                    ptr += sent;
                    remaining -= sent;
                }
            }
        }

        // For video, only send the NEWEST frame per sharer (drop older ones)
        // Use non-blocking sends so a slow subscriber doesn't stall others
        {
            std::unordered_map<uint32_t, size_t> newest; // sharer_id -> index
            for (size_t i = 0; i < frame_items.size(); ++i) {
                newest[frame_items[i].sharer_id] = i; // last one wins
            }
            for (auto& [id, idx] : newest) {
                auto& item = frame_items[idx];
                auto subs = get_subscriber_sockets(item.sharer_id);
                for (SOCKET s : subs) {
                    try_send_nonblocking(s, item.data.data(), item.data.size());
                }
            }
        }
    }
}

// ── Thread 3: Poll all client TCP sockets for LEAVE / disconnect ──
static void tcp_read_loop() {
    while (g_running) {
        std::vector<uint32_t> to_check;
        {
            std::lock_guard<std::mutex> lock(g_clients_mutex);
            for (auto& [id, c] : g_clients) {
                to_check.push_back(id);
            }
        }

        if (to_check.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        fd_set read_set;
        FD_ZERO(&read_set);

        {
            std::lock_guard<std::mutex> lock(g_clients_mutex);
            for (uint32_t id : to_check) {
                auto it = g_clients.find(id);
                if (it != g_clients.end() && it->second.tcp_socket.valid()) {
                    FD_SET(it->second.tcp_socket.get(), &read_set);
                }
            }
        }

        timeval timeout{};
        timeout.tv_sec  = 0;
        timeout.tv_usec = 200000;

        int ready = select(0, &read_set, nullptr, nullptr, &timeout);
        if (ready <= 0) continue;

        for (uint32_t id : to_check) {
            SOCKET s;
            {
                std::lock_guard<std::mutex> lock(g_clients_mutex);
                auto it = g_clients.find(id);
                if (it == g_clients.end() || !it->second.tcp_socket.valid()) continue;
                s = it->second.tcp_socket.get();
            }

            if (!FD_ISSET(s, &read_set)) continue;

            // Read header using raw SOCKET (no lock needed — only this thread
            // reads from client sockets, and remove_client is only called here)
            uint8_t hdr_buf[lilypad::SIGNAL_HEADER_SIZE];
            {
                int total = 0;
                while (total < static_cast<int>(lilypad::SIGNAL_HEADER_SIZE)) {
                    int r = recv(s, reinterpret_cast<char*>(hdr_buf + total),
                                 static_cast<int>(lilypad::SIGNAL_HEADER_SIZE) - total, 0);
                    if (r <= 0) { total = -1; break; }
                    total += r;
                }
                if (total < 0) { remove_client(id); continue; }
            }

            auto header = lilypad::deserialize_header(hdr_buf);

            // Read payload using raw SOCKET — no lock held during potentially
            // large reads (screen frames can be 100KB+)
            std::vector<uint8_t> payload;
            if (header.payload_len > 0) {
                payload.resize(header.payload_len);
                int total = 0;
                int target = static_cast<int>(header.payload_len);
                while (total < target) {
                    int r = recv(s, reinterpret_cast<char*>(payload.data() + total),
                                 target - total, 0);
                    if (r <= 0) { total = -1; break; }
                    total += r;
                }
                if (total < 0) { remove_client(id); continue; }
            }

            if (header.type == lilypad::MsgType::LEAVE) {
                remove_client(id);
            } else if (header.type == lilypad::MsgType::TEXT_CHAT && !payload.empty()) {
                // Extract the text, store it, and broadcast to all clients
                std::string text(reinterpret_cast<const char*>(payload.data()));
                {
                    std::lock_guard<std::mutex> chat_lock(g_chat_mutex);
                    g_chat_history.push_back({id, text});
                }
                auto broadcast = lilypad::make_text_chat_broadcast_msg(id, text);
                std::lock_guard<std::mutex> lock(g_clients_mutex);
                broadcast_tcp(broadcast);
            } else if (header.type == lilypad::MsgType::SCREEN_START) {
                std::lock_guard<std::mutex> lock(g_clients_mutex);
                auto it = g_clients.find(id);
                if (it != g_clients.end()) {
                    it->second.screen_sharing = true;
                    auto msg = lilypad::make_screen_start_broadcast(id);
                    broadcast_tcp(msg);
                }
            } else if (header.type == lilypad::MsgType::SCREEN_STOP) {
                std::lock_guard<std::mutex> lock(g_clients_mutex);
                auto it = g_clients.find(id);
                if (it != g_clients.end()) {
                    it->second.screen_sharing = false;
                    it->second.screen_subscribers.clear();
                    auto msg = lilypad::make_screen_stop_broadcast(id);
                    broadcast_tcp(msg);
                }
            } else if (header.type == lilypad::MsgType::SCREEN_SUBSCRIBE && payload.size() >= 4) {
                uint32_t target_id = lilypad::read_u32(payload.data());
                std::lock_guard<std::mutex> lock(g_clients_mutex);
                auto it = g_clients.find(target_id);
                if (it != g_clients.end() && it->second.screen_sharing) {
                    it->second.screen_subscribers.insert(id);
                }
            } else if (header.type == lilypad::MsgType::SCREEN_UNSUBSCRIBE && payload.size() >= 4) {
                uint32_t target_id = lilypad::read_u32(payload.data());
                std::lock_guard<std::mutex> lock(g_clients_mutex);
                auto it = g_clients.find(target_id);
                if (it != g_clients.end()) {
                    it->second.screen_subscribers.erase(id);
                }
            } else if (header.type == lilypad::MsgType::SCREEN_FRAME && payload.size() >= 4) {
                // Client sends: width(2) + height(2) + jpeg
                uint16_t w = lilypad::read_u16(payload.data());
                uint16_t h = lilypad::read_u16(payload.data() + 2);
                const uint8_t* jpeg = payload.data() + 4;
                size_t jpeg_len = payload.size() - 4;

                auto relay = lilypad::make_screen_frame_relay(id, w, h, jpeg, jpeg_len);
                enqueue_relay(std::move(relay), id, false);
            } else if (header.type == lilypad::MsgType::SCREEN_AUDIO && !payload.empty()) {
                auto relay = lilypad::make_screen_audio_relay(id, payload.data(), payload.size());
                enqueue_relay(std::move(relay), id, true);
            }
        }
    }
}

// ── Thread 3: UDP voice relay ──
static void udp_relay_loop(SOCKET udp_sock) {
    uint8_t buf[lilypad::MAX_VOICE_PACKET];

    while (g_running) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(udp_sock, &read_set);

        timeval timeout{};
        timeout.tv_sec  = 0;
        timeout.tv_usec = 200000;

        int ready = select(0, &read_set, nullptr, nullptr, &timeout);
        if (ready <= 0) continue;

        sockaddr_in sender_addr{};
        int addr_len = sizeof(sender_addr);
        int received = recvfrom(udp_sock, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                                reinterpret_cast<sockaddr*>(&sender_addr), &addr_len);

        if (received < static_cast<int>(lilypad::VOICE_HEADER_SIZE)) continue;

        uint32_t sender_id = lilypad::read_u32(buf);

        // Register the sender's UDP address on first packet
        {
            std::lock_guard<std::mutex> lock(g_clients_mutex);
            auto it = g_clients.find(sender_id);
            if (it == g_clients.end()) continue; // unknown client

            if (!it->second.udp_known) {
                it->second.udp_addr  = sender_addr;
                it->second.udp_known = true;
            }

            // Forward to all other clients with known UDP addresses
            for (auto& [id, client] : g_clients) {
                if (id == sender_id || !client.udp_known) continue;
                sendto(udp_sock, reinterpret_cast<const char*>(buf), received, 0,
                       reinterpret_cast<const sockaddr*>(&client.udp_addr),
                       sizeof(client.udp_addr));
            }
        }
    }
}

int main() {
    std::signal(SIGINT, signal_handler);
    load_update_config();

    try {
        lilypad::WinsockInit winsock;

        // ── Create and bind TCP listen socket ──
        auto tcp_listen = lilypad::create_tcp_socket();

        // Allow address reuse
        int opt = 1;
        setsockopt(tcp_listen.get(), SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&opt), sizeof(opt));

        sockaddr_in tcp_addr{};
        tcp_addr.sin_family      = AF_INET;
        tcp_addr.sin_addr.s_addr = INADDR_ANY;
        tcp_addr.sin_port        = htons(TCP_PORT);

        if (bind(tcp_listen.get(), reinterpret_cast<sockaddr*>(&tcp_addr),
                 sizeof(tcp_addr)) == SOCKET_ERROR) {
            std::cerr << "TCP bind failed: " << WSAGetLastError() << "\n";
            return 1;
        }

        if (listen(tcp_listen.get(), SOMAXCONN) == SOCKET_ERROR) {
            std::cerr << "TCP listen failed: " << WSAGetLastError() << "\n";
            return 1;
        }

        // ── Create and bind UDP socket ──
        auto udp_sock = lilypad::create_udp_socket();

        sockaddr_in udp_addr{};
        udp_addr.sin_family      = AF_INET;
        udp_addr.sin_addr.s_addr = INADDR_ANY;
        udp_addr.sin_port        = htons(UDP_PORT);

        if (bind(udp_sock.get(), reinterpret_cast<sockaddr*>(&udp_addr),
                 sizeof(udp_addr)) == SOCKET_ERROR) {
            std::cerr << "UDP bind failed: " << WSAGetLastError() << "\n";
            return 1;
        }

        std::cout << "Listening on TCP port " << TCP_PORT
                  << ", UDP port " << UDP_PORT << "\n";

        // ── Launch threads ──
        std::thread tcp_accept_thread(tcp_accept_loop, tcp_listen.get());
        std::thread tcp_read_thread(tcp_read_loop);
        std::thread udp_relay_thread(udp_relay_loop, udp_sock.get());
        std::thread screen_relay_thread(screen_relay_loop);

        // Wait for Ctrl+C
        tcp_accept_thread.join();
        tcp_read_thread.join();
        udp_relay_thread.join();
        g_relay_cv.notify_all();
        screen_relay_thread.join();

        std::cout << "[Server] Shutting down.\n";

    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
