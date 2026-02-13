#include "auth_db.h"
#include "chat_persistence.h"
#include "network.h"
#include "protocol.h"
#include "tls_config.h"
#include "tls_socket.h"

#include <sodium.h>

#include <atomic>
#include <chrono>
#include <ctime>
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
    uint32_t           id = 0;
    std::string        username;
    lilypad::TlsSocket tls_socket;
    sockaddr_in        udp_addr{};   // filled in when first UDP packet arrives
    bool               udp_known = false;
    int64_t            db_user_id = 0;

    // Voice channel
    bool               in_voice = false;

    // Screen sharing
    bool                             screen_sharing = false;
    std::unordered_set<uint32_t>     screen_subscribers; // IDs of clients watching this user
    std::vector<uint8_t>             cached_keyframe;    // last H.264 keyframe relay msg
};

static std::mutex                                   g_clients_mutex;
static std::unordered_map<uint32_t, ClientInfo>     g_clients;
static uint32_t                                     g_next_id = 1;

// ── Auth database ──
static std::unique_ptr<lilypad::AuthDB> g_auth_db;

// ── TLS ──
static SSL_CTX* g_ssl_ctx = nullptr;

// ── Rate limiting (per-IP) ──
struct RateLimitEntry {
    int      failures = 0;
    std::chrono::steady_clock::time_point window_start;
};
static std::mutex g_rate_limit_mutex;
static std::unordered_map<std::string, RateLimitEntry> g_rate_limits;
constexpr int    RATE_LIMIT_MAX_FAILURES = 5;
constexpr int    RATE_LIMIT_WINDOW_SECS  = 60;

static bool check_rate_limit(const std::string& ip) {
    std::lock_guard<std::mutex> lock(g_rate_limit_mutex);
    auto now = std::chrono::steady_clock::now();
    auto& entry = g_rate_limits[ip];

    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - entry.window_start).count();
    if (elapsed >= RATE_LIMIT_WINDOW_SECS) {
        entry.failures = 0;
        entry.window_start = now;
    }

    return entry.failures < RATE_LIMIT_MAX_FAILURES;
}

static void record_auth_failure(const std::string& ip) {
    std::lock_guard<std::mutex> lock(g_rate_limit_mutex);
    auto now = std::chrono::steady_clock::now();
    auto& entry = g_rate_limits[ip];

    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - entry.window_start).count();
    if (elapsed >= RATE_LIMIT_WINDOW_SECS) {
        entry.failures = 0;
        entry.window_start = now;
    }
    entry.failures++;
}

// ── Chat history (persistent across restarts via chat_history.jsonl) ──
struct ChatEntry {
    uint64_t    seq       = 0;
    std::string sender_name;
    int64_t     timestamp = 0;
    std::string text;
};
static std::mutex                g_chat_mutex;
static std::vector<ChatEntry>    g_chat_history;
static uint64_t                  g_next_seq = 1;
static const char*               CHAT_HISTORY_FILE = "chat_history.jsonl";

static void load_chat_history() {
    std::ifstream file(CHAT_HISTORY_FILE);
    if (!file.is_open()) return;
    std::string line;
    uint64_t max_seq = 0;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        auto entry = lilypad::parse_chat_line(line);
        if (!entry.valid) continue;
        ChatEntry ce;
        ce.seq         = entry.seq;
        ce.sender_name = entry.sender;
        ce.timestamp   = entry.timestamp;
        ce.text        = entry.text;
        g_chat_history.push_back(std::move(ce));
        if (entry.seq > max_seq) max_seq = entry.seq;
    }
    g_next_seq = max_seq + 1;
    std::cout << "[Server] Loaded " << g_chat_history.size() << " chat messages (next seq=" << g_next_seq << ")\n";
}

static void append_chat_to_file(const ChatEntry& entry) {
    std::ofstream file(CHAT_HISTORY_FILE, std::ios::app);
    if (file.is_open()) {
        file << lilypad::serialize_chat_line(entry.seq, entry.sender_name, entry.timestamp, entry.text) << '\n';
        file.flush();
    }
}

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
    bool                 is_audio;     // true = SCREEN_AUDIO (high priority)
    bool                 is_keyframe;  // true = H.264 IDR (don't drop)
};

static std::mutex               g_relay_mutex;
static std::condition_variable  g_relay_cv;
static std::deque<RelayItem>    g_relay_queue;

static void enqueue_relay(std::vector<uint8_t> data, uint32_t sharer_id, bool is_audio, bool is_keyframe = false) {
    {
        std::lock_guard<std::mutex> lock(g_relay_mutex);
        g_relay_queue.push_back({std::move(data), sharer_id, is_audio, is_keyframe});
        // Limit queue depth — drop oldest non-audio, non-keyframe items to prevent unbounded growth
        while (g_relay_queue.size() > 60) {
            bool dropped = false;
            for (auto it = g_relay_queue.begin(); it != g_relay_queue.end(); ++it) {
                if (!it->is_audio && !it->is_keyframe) {
                    g_relay_queue.erase(it);
                    dropped = true;
                    break;
                }
            }
            if (!dropped) break; // all items are audio or keyframes, stop dropping
        }
    }
    g_relay_cv.notify_one();
}

// ── Broadcast a TCP message to all clients (caller must hold g_clients_mutex) ──
static void broadcast_tcp(const std::vector<uint8_t>& msg) {
    for (auto& [id, client] : g_clients) {
        client.tls_socket.send_all(msg);
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

        // If this client was in voice, broadcast VOICE_LEFT
        if (it->second.in_voice) {
            auto voice_left = lilypad::make_voice_left_broadcast(client_id);
            for (auto& [id, c] : g_clients) {
                if (id != client_id)
                    c.tls_socket.send_all(voice_left);
            }
        }

        // If this client was sharing, broadcast SCREEN_STOP
        if (it->second.screen_sharing) {
            auto stop_msg = lilypad::make_screen_stop_broadcast(client_id);
            for (auto& [id, c] : g_clients) {
                if (id != client_id)
                    c.tls_socket.send_all(stop_msg);
            }
        }

        // Remove this client from all subscriber sets
        for (auto& [id, c] : g_clients) {
            c.screen_subscribers.erase(client_id);
        }

        it->second.tls_socket.close();
        g_clients.erase(it);

        // Broadcast USER_LEFT to remaining clients
        auto msg = lilypad::make_user_left_msg(client_id);
        broadcast_tcp(msg);
    }
    std::cout << "[Server] " << name << " (id=" << client_id << ") left.\n";
}

// ── Per-client read threads (forward declarations for tcp_accept_loop) ──
static std::mutex               g_client_threads_mutex;
static std::vector<std::thread> g_client_threads;
static void client_read_loop(uint32_t id);

// ── Helper: complete post-auth setup for an authenticated client ──
// Caller must NOT hold g_clients_mutex. Returns true on success.
static bool setup_authenticated_client(lilypad::TlsSocket&& tls, const std::string& username,
                                        int64_t db_user_id, uint32_t& out_client_id) {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    uint32_t client_id = g_next_id++;

    // Send update notification if configured
    if (!g_update_version.empty() && !g_update_url.empty()) {
        auto update_msg = lilypad::make_update_available_msg(g_update_version, g_update_url);
        tls.send_all(update_msg);
    }

    // Send existing user list
    for (auto& [id, existing] : g_clients) {
        auto msg = lilypad::make_user_joined_msg(existing.id, existing.username);
        tls.send_all(msg);
    }

    // Send SCREEN_START for any currently-sharing users
    for (auto& [id, existing] : g_clients) {
        if (existing.screen_sharing) {
            auto msg = lilypad::make_screen_start_broadcast(existing.id);
            tls.send_all(msg);
        }
    }

    // Send VOICE_JOINED for any users currently in voice
    for (auto& [id, existing] : g_clients) {
        if (existing.in_voice) {
            auto msg = lilypad::make_voice_joined_broadcast(existing.id);
            tls.send_all(msg);
        }
    }

    // Broadcast USER_JOINED to all existing clients
    auto joined_msg = lilypad::make_user_joined_msg(client_id, username);
    broadcast_tcp(joined_msg);

    // Add the new client
    ClientInfo info;
    info.id          = client_id;
    info.username    = username;
    info.tls_socket  = std::move(tls);
    info.udp_known   = false;
    info.db_user_id  = db_user_id;
    g_clients[client_id] = std::move(info);

    out_client_id = client_id;
    return true;
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

        // Large send buffer
        int sndbuf = 1024 * 1024;
        setsockopt(new_sock, SOL_SOCKET, SO_SNDBUF,
                   reinterpret_cast<const char*>(&sndbuf), sizeof(sndbuf));

        // Large receive buffer
        int rcvbuf = 1024 * 1024;
        setsockopt(new_sock, SOL_SOCKET, SO_RCVBUF,
                   reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));

        lilypad::Socket raw_socket(new_sock);
        lilypad::TlsSocket tls;

        // TLS handshake
        if (!tls.accept(std::move(raw_socket), g_ssl_ctx)) {
            std::cout << "[Server] TLS handshake failed from " << inet_ntoa(client_addr.sin_addr) << "\n";
            continue;
        }

        std::string peer_ip = tls.peer_ip();

        // Auth handshake loop -- client can register then login, or just login
        bool authenticated = false;
        uint32_t client_id = 0;

        while (g_running && !authenticated) {
            // Read signal header
            uint8_t hdr_buf[lilypad::SIGNAL_HEADER_SIZE];
            if (!tls.recv_all(hdr_buf, lilypad::SIGNAL_HEADER_SIZE)) {
                break; // connection lost
            }

            auto header = lilypad::deserialize_header(hdr_buf);

            // Read payload
            std::vector<uint8_t> payload;
            if (header.payload_len > 0) {
                if (header.payload_len > 4096) break; // auth messages should be small
                payload.resize(header.payload_len);
                if (!tls.recv_all(payload.data(), header.payload_len)) break;
            }

            if (header.type == lilypad::MsgType::AUTH_REGISTER_REQ) {
                // Parse: username\0 + password\0
                const char* p = reinterpret_cast<const char*>(payload.data());
                std::string username(p);
                size_t pass_offset = username.size() + 1;
                if (pass_offset >= payload.size()) {
                    auto resp = lilypad::make_auth_register_resp(lilypad::AuthStatus::ERR_INVALID_INPUT,
                                                                  "Invalid request");
                    tls.send_all(resp);
                    continue;
                }
                std::string password(reinterpret_cast<const char*>(payload.data() + pass_offset));

                // Validate input
                if (!lilypad::is_valid_username(username)) {
                    auto resp = lilypad::make_auth_register_resp(lilypad::AuthStatus::ERR_INVALID_INPUT,
                                                                  "Username must be 1-32 alphanumeric/underscore characters");
                    tls.send_all(resp);
                    continue;
                }
                if (!lilypad::is_valid_password(password)) {
                    auto resp = lilypad::make_auth_register_resp(lilypad::AuthStatus::ERR_INVALID_INPUT,
                                                                  "Password must be 8-128 characters");
                    tls.send_all(resp);
                    continue;
                }

                auto result = g_auth_db->register_user(username, password);
                auto status = result.success ? lilypad::AuthStatus::OK : lilypad::AuthStatus::ERR_USERNAME_TAKEN;
                auto resp = lilypad::make_auth_register_resp(status, result.message);
                tls.send_all(resp);
                // Don't break -- client should now send a login request
                continue;

            } else if (header.type == lilypad::MsgType::AUTH_LOGIN_REQ) {
                // Rate limit check
                if (!check_rate_limit(peer_ip)) {
                    auto resp = lilypad::make_auth_login_resp(lilypad::AuthStatus::ERR_RATE_LIMITED,
                                                               0, 0, std::vector<uint8_t>(32, 0).data(),
                                                               "Too many failed attempts. Try again later.");
                    tls.send_all(resp);
                    continue;
                }

                // Parse: username\0 + password\0
                const char* p = reinterpret_cast<const char*>(payload.data());
                std::string username(p);
                size_t pass_offset = username.size() + 1;
                if (pass_offset >= payload.size()) {
                    auto resp = lilypad::make_auth_login_resp(lilypad::AuthStatus::ERR_INVALID_INPUT,
                                                               0, 0, std::vector<uint8_t>(32, 0).data(),
                                                               "Invalid request");
                    tls.send_all(resp);
                    continue;
                }
                std::string password(reinterpret_cast<const char*>(payload.data() + pass_offset));

                auto result = g_auth_db->verify_login(username, password);
                if (!result.success) {
                    record_auth_failure(peer_ip);
                    auto resp = lilypad::make_auth_login_resp(lilypad::AuthStatus::ERR_INVALID_CREDS,
                                                               0, 0, std::vector<uint8_t>(32, 0).data(),
                                                               result.message);
                    tls.send_all(resp);
                    continue;
                }

                // Create session token
                auto token = g_auth_db->create_session(result.user_id);

                // Setup client
                if (!setup_authenticated_client(std::move(tls), username, result.user_id, client_id)) {
                    break;
                }

                // Send AUTH_LOGIN_RESP (replaces WELCOME)
                {
                    std::lock_guard<std::mutex> lock(g_clients_mutex);
                    auto it = g_clients.find(client_id);
                    if (it != g_clients.end()) {
                        auto resp = lilypad::make_auth_login_resp(lilypad::AuthStatus::OK,
                                                                    client_id, UDP_PORT, token.data(),
                                                                    "Login successful");
                        it->second.tls_socket.send_all(resp);
                    }
                }

                authenticated = true;
                std::cout << "[Server] " << username << " (id=" << client_id << ") authenticated.\n";

            } else if (header.type == lilypad::MsgType::AUTH_TOKEN_LOGIN_REQ) {
                // Rate limit check
                if (!check_rate_limit(peer_ip)) {
                    auto resp = lilypad::make_auth_token_login_resp(lilypad::AuthStatus::ERR_RATE_LIMITED,
                                                                     0, 0, std::vector<uint8_t>(32, 0).data(),
                                                                     "Too many failed attempts. Try again later.");
                    tls.send_all(resp);
                    continue;
                }

                // Parse: username\0 + token(32)
                const char* p = reinterpret_cast<const char*>(payload.data());
                std::string username(p);
                size_t token_offset = username.size() + 1;
                if (token_offset + lilypad::SESSION_TOKEN_SIZE > payload.size()) {
                    auto resp = lilypad::make_auth_token_login_resp(lilypad::AuthStatus::ERR_INVALID_INPUT,
                                                                     0, 0, std::vector<uint8_t>(32, 0).data(),
                                                                     "Invalid request");
                    tls.send_all(resp);
                    continue;
                }
                const uint8_t* raw_token = payload.data() + token_offset;

                auto result = g_auth_db->validate_token(username, raw_token);
                if (!result.success) {
                    record_auth_failure(peer_ip);
                    auto resp = lilypad::make_auth_token_login_resp(lilypad::AuthStatus::ERR_TOKEN_EXPIRED,
                                                                     0, 0, std::vector<uint8_t>(32, 0).data(),
                                                                     result.message);
                    tls.send_all(resp);
                    continue;
                }

                // Setup client
                if (!setup_authenticated_client(std::move(tls), result.username, result.user_id, client_id)) {
                    break;
                }

                // Send AUTH_TOKEN_LOGIN_RESP
                {
                    std::lock_guard<std::mutex> lock(g_clients_mutex);
                    auto it = g_clients.find(client_id);
                    if (it != g_clients.end()) {
                        auto resp = lilypad::make_auth_token_login_resp(lilypad::AuthStatus::OK,
                                                                         client_id, UDP_PORT, result.new_token.data(),
                                                                         "Token login successful");
                        it->second.tls_socket.send_all(resp);
                    }
                }

                authenticated = true;
                std::cout << "[Server] " << result.username << " (id=" << client_id << ") token-authenticated.\n";

            } else {
                // Unknown message type during auth handshake, reject
                break;
            }
        }

        if (!authenticated) continue;

        // Spawn dedicated read thread
        {
            std::lock_guard<std::mutex> lock(g_client_threads_mutex);
            g_client_threads.emplace_back(client_read_loop, client_id);
        }
    }
}

// ── Screen relay thread helper: send with timeout via SO_SNDTIMEO ──
// TLS doesn't work with non-blocking send approach, so use send timeout instead.
static bool tls_send_with_timeout(lilypad::TlsSocket& tls, const uint8_t* data, size_t len) {
    // SO_SNDTIMEO is set on the socket level, SSL_write will respect it
    return tls.send_all(data, len);
}

// ── Thread 2: Dedicated screen relay thread ──
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

            for (auto& item : g_relay_queue) {
                if (item.is_audio)
                    audio_items.push_back(std::move(item));
                else
                    frame_items.push_back(std::move(item));
            }
            g_relay_queue.clear();
        }

        // Helper: collect subscriber TLS sockets with SO_SNDTIMEO set
        auto send_to_subscribers = [](uint32_t sharer_id, const uint8_t* data, size_t len, bool set_timeout) {
            std::lock_guard<std::mutex> lock(g_clients_mutex);
            auto it = g_clients.find(sharer_id);
            if (it == g_clients.end()) return;
            for (uint32_t sub_id : it->second.screen_subscribers) {
                auto sub_it = g_clients.find(sub_id);
                if (sub_it == g_clients.end()) continue;

                if (set_timeout) {
                    // Set 50ms send timeout for video frames
                    DWORD timeout_ms = 50;
                    setsockopt(sub_it->second.tls_socket.get(), SOL_SOCKET, SO_SNDTIMEO,
                               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
                }

                sub_it->second.tls_socket.send_all(data, len);

                if (set_timeout) {
                    // Reset send timeout
                    DWORD timeout_ms = 0;
                    setsockopt(sub_it->second.tls_socket.get(), SOL_SOCKET, SO_SNDTIMEO,
                               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
                }
            }
        };

        // Send audio first (high priority) — blocking sends (small packets)
        for (auto& item : audio_items) {
            send_to_subscribers(item.sharer_id, item.data.data(), item.data.size(), false);
        }

        // For video, only send the NEWEST frame per sharer
        {
            std::unordered_map<uint32_t, size_t> newest;
            for (size_t i = 0; i < frame_items.size(); ++i) {
                newest[frame_items[i].sharer_id] = i;
            }
            for (auto& [id, idx] : newest) {
                auto& item = frame_items[idx];
                send_to_subscribers(item.sharer_id, item.data.data(), item.data.size(), true);
            }
        }
    }
}

// ── Per-client read loop ──
static void client_read_loop(uint32_t id) {
    while (g_running) {
        SOCKET raw_sock = INVALID_SOCKET;
        {
            std::lock_guard<std::mutex> lock(g_clients_mutex);
            auto it = g_clients.find(id);
            if (it == g_clients.end()) return;
            raw_sock = it->second.tls_socket.get();
        }
        if (raw_sock == INVALID_SOCKET) return;

        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(raw_sock, &read_set);

        timeval timeout{};
        timeout.tv_sec  = 0;
        timeout.tv_usec = 200000; // 200ms

        int ready = select(0, &read_set, nullptr, nullptr, &timeout);
        if (ready <= 0) continue;

        // Read header via TLS
        uint8_t hdr_buf[lilypad::SIGNAL_HEADER_SIZE];
        {
            std::lock_guard<std::mutex> lock(g_clients_mutex);
            auto it = g_clients.find(id);
            if (it == g_clients.end()) return;
            if (!it->second.tls_socket.recv_all(hdr_buf, lilypad::SIGNAL_HEADER_SIZE)) {
                lock.~lock_guard();
                remove_client(id);
                return;
            }
        }

        auto header = lilypad::deserialize_header(hdr_buf);

        // Read payload
        std::vector<uint8_t> payload;
        if (header.payload_len > 0) {
            payload.resize(header.payload_len);
            std::lock_guard<std::mutex> lock(g_clients_mutex);
            auto it = g_clients.find(id);
            if (it == g_clients.end()) return;
            if (!it->second.tls_socket.recv_all(payload.data(), header.payload_len)) {
                lock.~lock_guard();
                remove_client(id);
                return;
            }
        }

        if (header.type == lilypad::MsgType::LEAVE) {
            remove_client(id);
            return;
        } else if (header.type == lilypad::MsgType::TEXT_CHAT && !payload.empty()) {
            std::string text(reinterpret_cast<const char*>(payload.data()));
            std::string sender_name;
            {
                std::lock_guard<std::mutex> lock(g_clients_mutex);
                auto cit = g_clients.find(id);
                if (cit != g_clients.end())
                    sender_name = cit->second.username;
                else
                    sender_name = "User #" + std::to_string(id);
            }
            ChatEntry ce;
            int64_t now_ts = static_cast<int64_t>(std::time(nullptr));
            {
                std::lock_guard<std::mutex> chat_lock(g_chat_mutex);
                ce.seq         = g_next_seq++;
                ce.sender_name = sender_name;
                ce.timestamp   = now_ts;
                ce.text        = text;
                g_chat_history.push_back(ce);
            }
            append_chat_to_file(ce);
            auto broadcast = lilypad::make_text_chat_broadcast_v2(
                ce.seq, id, ce.timestamp, ce.sender_name, ce.text);
            std::lock_guard<std::mutex> lock(g_clients_mutex);
            broadcast_tcp(broadcast);
        } else if (header.type == lilypad::MsgType::VOICE_JOIN) {
            std::lock_guard<std::mutex> lock(g_clients_mutex);
            auto it = g_clients.find(id);
            if (it != g_clients.end()) {
                it->second.in_voice = true;
                auto msg = lilypad::make_voice_joined_broadcast(id);
                broadcast_tcp(msg);
            }
        } else if (header.type == lilypad::MsgType::VOICE_LEAVE) {
            std::lock_guard<std::mutex> lock(g_clients_mutex);
            auto it = g_clients.find(id);
            if (it != g_clients.end()) {
                it->second.in_voice = false;
                auto msg = lilypad::make_voice_left_broadcast(id);
                broadcast_tcp(msg);
            }
        } else if (header.type == lilypad::MsgType::CHAT_SYNC && payload.size() >= 8) {
            uint64_t last_seq = lilypad::read_u64(payload.data());
            std::lock_guard<std::mutex> chat_lock(g_chat_mutex);
            for (auto& entry : g_chat_history) {
                if (entry.seq > last_seq) {
                    auto msg = lilypad::make_text_chat_broadcast_v2(
                        entry.seq, 0, entry.timestamp, entry.sender_name, entry.text);
                    std::lock_guard<std::mutex> lock(g_clients_mutex);
                    auto cit = g_clients.find(id);
                    if (cit != g_clients.end()) {
                        cit->second.tls_socket.send_all(msg);
                    }
                }
            }
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
                it->second.cached_keyframe.clear();
                auto msg = lilypad::make_screen_stop_broadcast(id);
                broadcast_tcp(msg);
            }
        } else if (header.type == lilypad::MsgType::SCREEN_SUBSCRIBE && payload.size() >= 4) {
            uint32_t target_id = lilypad::read_u32(payload.data());
            std::lock_guard<std::mutex> lock(g_clients_mutex);
            auto it = g_clients.find(target_id);
            if (it != g_clients.end() && it->second.screen_sharing) {
                it->second.screen_subscribers.insert(id);
                auto sub_it = g_clients.find(id);
                if (sub_it != g_clients.end()) {
                    if (!it->second.cached_keyframe.empty()) {
                        sub_it->second.tls_socket.send_all(it->second.cached_keyframe);
                    } else {
                        auto req = lilypad::make_screen_request_keyframe_msg();
                        it->second.tls_socket.send_all(req);
                    }
                }
            }
        } else if (header.type == lilypad::MsgType::SCREEN_UNSUBSCRIBE && payload.size() >= 4) {
            uint32_t target_id = lilypad::read_u32(payload.data());
            std::lock_guard<std::mutex> lock(g_clients_mutex);
            auto it = g_clients.find(target_id);
            if (it != g_clients.end()) {
                it->second.screen_subscribers.erase(id);
            }
        } else if (header.type == lilypad::MsgType::SCREEN_FRAME && payload.size() >= 5) {
            uint16_t w = lilypad::read_u16(payload.data());
            uint16_t h = lilypad::read_u16(payload.data() + 2);
            uint8_t flags = payload[4];
            const uint8_t* frame_data = payload.data() + 5;
            size_t frame_len = payload.size() - 5;

            bool is_keyframe = (flags & lilypad::SCREEN_FLAG_KEYFRAME) != 0;

            auto relay = lilypad::make_screen_frame_relay(id, w, h, flags, frame_data, frame_len);

            if (is_keyframe) {
                std::lock_guard<std::mutex> lock(g_clients_mutex);
                auto it = g_clients.find(id);
                if (it != g_clients.end()) {
                    it->second.cached_keyframe = relay;
                }
            }

            enqueue_relay(std::move(relay), id, false, is_keyframe);
        } else if (header.type == lilypad::MsgType::SCREEN_AUDIO && !payload.empty()) {
            auto relay = lilypad::make_screen_audio_relay(id, payload.data(), payload.size());
            enqueue_relay(std::move(relay), id, true);
        } else if (header.type == lilypad::MsgType::AUTH_CHANGE_PASS_REQ) {
            // Parse: old_password\0 + new_password\0
            const char* p = reinterpret_cast<const char*>(payload.data());
            std::string old_pass(p);
            size_t new_offset = old_pass.size() + 1;
            std::string new_pass;
            if (new_offset < payload.size()) {
                new_pass = std::string(reinterpret_cast<const char*>(payload.data() + new_offset));
            }

            int64_t db_user_id = 0;
            {
                std::lock_guard<std::mutex> lock(g_clients_mutex);
                auto it = g_clients.find(id);
                if (it != g_clients.end()) db_user_id = it->second.db_user_id;
            }

            if (!lilypad::is_valid_password(new_pass)) {
                auto resp = lilypad::make_auth_change_pass_resp(lilypad::AuthStatus::ERR_INVALID_INPUT,
                                                                 "Password must be 8-128 characters");
                std::lock_guard<std::mutex> lock(g_clients_mutex);
                auto it = g_clients.find(id);
                if (it != g_clients.end()) it->second.tls_socket.send_all(resp);
            } else {
                auto result = g_auth_db->change_password(db_user_id, old_pass, new_pass);
                auto status = result.success ? lilypad::AuthStatus::OK : lilypad::AuthStatus::ERR_INVALID_CREDS;
                auto resp = lilypad::make_auth_change_pass_resp(status, result.message);
                std::lock_guard<std::mutex> lock(g_clients_mutex);
                auto it = g_clients.find(id);
                if (it != g_clients.end()) it->second.tls_socket.send_all(resp);
            }
        } else if (header.type == lilypad::MsgType::AUTH_DELETE_ACCT_REQ) {
            const char* p = reinterpret_cast<const char*>(payload.data());
            std::string password(p);

            int64_t db_user_id = 0;
            {
                std::lock_guard<std::mutex> lock(g_clients_mutex);
                auto it = g_clients.find(id);
                if (it != g_clients.end()) db_user_id = it->second.db_user_id;
            }

            auto result = g_auth_db->delete_account(db_user_id, password);
            auto status = result.success ? lilypad::AuthStatus::OK : lilypad::AuthStatus::ERR_INVALID_CREDS;
            auto resp = lilypad::make_auth_delete_acct_resp(status, result.message);
            {
                std::lock_guard<std::mutex> lock(g_clients_mutex);
                auto it = g_clients.find(id);
                if (it != g_clients.end()) it->second.tls_socket.send_all(resp);
            }

            if (result.success) {
                remove_client(id);
                return;
            }
        } else if (header.type == lilypad::MsgType::AUTH_LOGOUT) {
            // Invalidate all sessions for this user
            int64_t db_user_id = 0;
            {
                std::lock_guard<std::mutex> lock(g_clients_mutex);
                auto it = g_clients.find(id);
                if (it != g_clients.end()) db_user_id = it->second.db_user_id;
            }
            if (db_user_id > 0) {
                g_auth_db->invalidate_all_sessions(db_user_id);
            }
            remove_client(id);
            return;
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

            // Only relay voice if sender is in voice channel
            if (!it->second.in_voice) continue;

            // Forward to all other clients with known UDP addresses that are in voice
            for (auto& [cid, client] : g_clients) {
                if (cid == sender_id || !client.udp_known || !client.in_voice) continue;
                sendto(udp_sock, reinterpret_cast<const char*>(buf), received, 0,
                       reinterpret_cast<const sockaddr*>(&client.udp_addr),
                       sizeof(client.udp_addr));
            }
        }
    }
}

// ── Session cleanup thread ──
static void session_cleanup_loop() {
    while (g_running) {
        // Sleep for 1 hour, checking g_running every second
        for (int i = 0; i < 3600 && g_running; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (!g_running) break;
        g_auth_db->cleanup_expired_sessions();
    }
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signal_handler);

    // Parse CLI args
    std::string cert_path = "server.crt";
    std::string key_path  = "server.key";
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--cert" && i + 1 < argc) cert_path = argv[++i];
        else if (arg == "--key" && i + 1 < argc) key_path = argv[++i];
    }

    load_update_config();
    load_chat_history();

    try {
        lilypad::WinsockInit winsock;
        lilypad::OpenSSLInit openssl;

        // Initialize libsodium
        if (sodium_init() < 0) {
            std::cerr << "Failed to initialize libsodium\n";
            return 1;
        }

        // Initialize auth database
        g_auth_db = std::make_unique<lilypad::AuthDB>("lilypad.db");
        g_auth_db->cleanup_expired_sessions();

        // Load or generate TLS certificate
        if (!lilypad::load_or_generate_cert(cert_path, key_path)) {
            std::cerr << "Failed to load/generate TLS certificate\n";
            return 1;
        }
        g_ssl_ctx = lilypad::create_server_ssl_ctx(cert_path, key_path);
        if (!g_ssl_ctx) {
            std::cerr << "Failed to create SSL context\n";
            return 1;
        }

        // ── Create and bind TCP listen socket ──
        auto tcp_listen = lilypad::create_tcp_socket();

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
                  << ", UDP port " << UDP_PORT << " (TLS enabled)\n";

        // ── Launch threads ──
        std::thread tcp_accept_thread(tcp_accept_loop, tcp_listen.get());
        std::thread udp_relay_thread(udp_relay_loop, udp_sock.get());
        std::thread screen_relay_thread(screen_relay_loop);
        std::thread cleanup_thread(session_cleanup_loop);

        // Wait for Ctrl+C
        tcp_accept_thread.join();

        // Join all per-client read threads
        {
            std::lock_guard<std::mutex> lock(g_client_threads_mutex);
            for (auto& t : g_client_threads) {
                if (t.joinable()) t.join();
            }
        }

        udp_relay_thread.join();
        g_relay_cv.notify_all();
        screen_relay_thread.join();
        cleanup_thread.join();

        if (g_ssl_ctx) SSL_CTX_free(g_ssl_ctx);
        g_auth_db.reset();

        std::cout << "[Server] Shutting down.\n";

    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
