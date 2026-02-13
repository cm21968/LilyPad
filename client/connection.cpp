#include "connection.h"
#include "persistence.h"
#include "network_threads.h"
#include "screen_threads.h"
#include "chat_persistence.h"

#include <fstream>

// ── Shared post-auth setup (common to login and token login) ──
static void post_auth_setup(AppState& app, uint32_t my_id, uint16_t udp_port,
                             const uint8_t* token, const std::string& server_ip) {
    auto udp = std::make_unique<lilypad::Socket>(lilypad::create_udp_socket());

    sockaddr_in udp_dest{};
    udp_dest.sin_family = AF_INET;
    udp_dest.sin_port   = htons(udp_port);
    inet_pton(AF_INET, server_ip.c_str(), &udp_dest.sin_addr);

    app.udp      = std::move(udp);
    app.udp_dest = udp_dest;
    app.my_id    = my_id;
    app.in_voice = false;

    // Store session token
    app.session_token.assign(token, token + lilypad::SESSION_TOKEN_SIZE);

    {
        std::lock_guard<std::mutex> lk(app.users_mutex);
        app.users.clear();
    }
    {
        std::lock_guard<std::mutex> lk(app.volume_mutex);
        app.user_volumes.clear();
    }
    {
        std::lock_guard<std::mutex> lk(app.jitter_mutex);
        app.jitter_buffers.clear();
        app.voice_decoders.clear();
    }

    // Reset screen sharing state
    app.screen_sharing = false;
    app.watching_user_id = 0;
    app.force_keyframe = false;
    {
        std::lock_guard<std::mutex> lk(app.screen_frame_mutex);
        app.screen_frame_buf.clear();
        app.screen_frame_flags = 0;
        app.screen_frame_new = false;
    }

    // Load chat cache from disk
    app.last_known_seq = 0;
    {
        std::lock_guard<std::mutex> lk(app.chat_mutex);
        app.chat_messages.clear();
    }
    std::string cache_path = get_chat_cache_path(server_ip);
    if (!cache_path.empty()) {
        std::ifstream cf(cache_path);
        std::string line;
        while (std::getline(cf, line)) {
            if (line.empty()) continue;
            auto entry = lilypad::parse_chat_line(line);
            if (!entry.valid) continue;
            app.add_chat_msg(0, entry.sender, entry.text, entry.seq, entry.timestamp);
            if (entry.seq > app.last_known_seq.load())
                app.last_known_seq = entry.seq;
        }
    }

    app.auth_state = AuthState::AUTHENTICATED;
    app.connected = true;
    app.add_system_msg("Connected! Your ID: " + std::to_string(my_id));

    // Send CHAT_SYNC
    auto sync_msg = lilypad::make_chat_sync_msg(app.last_known_seq.load());
    app.send_tcp(sync_msg);

    // Start TCP receive and screen decode threads
    app.tcp_thread = std::make_unique<std::thread>(tcp_receive_thread, std::ref(app));
    app.screen_decode_thread = std::make_unique<std::thread>(screen_decode_thread_func, std::ref(app));
}

void do_tls_connect(AppState& app, const std::string& server_ip) {
    try {
        auto tcp_raw = std::make_unique<lilypad::Socket>(lilypad::create_tcp_socket());

        // Disable Nagle's algorithm
        int nodelay = 1;
        setsockopt(tcp_raw->get(), IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

        // Large send buffer
        int sndbuf = 1024 * 1024;
        setsockopt(tcp_raw->get(), SOL_SOCKET, SO_SNDBUF,
                   reinterpret_cast<const char*>(&sndbuf), sizeof(sndbuf));

        // Large receive buffer
        int rcvbuf = 1024 * 1024;
        setsockopt(tcp_raw->get(), SOL_SOCKET, SO_RCVBUF,
                   reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));

        // Resolve hostname (supports both IP addresses and domain names)
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* result = nullptr;
        if (getaddrinfo(server_ip.c_str(), "7777", &hints, &result) != 0 || !result) {
            app.add_system_msg("Failed to resolve: " + server_ip);
            return;
        }

        if (connect(tcp_raw->get(), result->ai_addr,
                    static_cast<int>(result->ai_addrlen)) == SOCKET_ERROR) {
            int err = WSAGetLastError();
            freeaddrinfo(result);
            app.add_system_msg("Failed to connect: error " + std::to_string(err));
            return;
        }
        freeaddrinfo(result);

        // Create client SSL context
        SSL_CTX* client_ctx = lilypad::create_client_ssl_ctx(app.trust_self_signed);
        if (!client_ctx) {
            app.add_system_msg("Failed to create TLS context.");
            return;
        }

        auto tls = std::make_unique<lilypad::TlsSocket>();
        if (!tls->connect(std::move(*tcp_raw), client_ctx)) {
            SSL_CTX_free(client_ctx);
            app.add_system_msg("TLS handshake failed. Server may use a self-signed certificate.");
            return;
        }
        SSL_CTX_free(client_ctx);

        app.tcp = std::move(tls);
        app.server_ip = server_ip;
        app.auth_state = AuthState::CONNECTED_UNAUTH;
        app.add_system_msg("TLS connected. Please log in or register.");

    } catch (const std::exception& e) {
        app.add_system_msg(std::string("Connection error: ") + e.what());
    }
}

void do_login(AppState& app, const std::string& username, const std::string& password, bool remember_me) {
    if (app.auth_state.load() != AuthState::CONNECTED_UNAUTH) return;

    app.auth_state = AuthState::LOGGING_IN;
    app.my_username = username;

    auto req = lilypad::make_auth_login_req(username, password);
    {
        std::lock_guard<std::mutex> lk(app.tcp_send_mutex);
        if (!app.tcp || !app.tcp->send_all(req)) {
            app.auth_state = AuthState::CONNECTED_UNAUTH;
            app.add_system_msg("Failed to send login request.");
            return;
        }
    }

    // Read response
    uint8_t hdr_buf[lilypad::SIGNAL_HEADER_SIZE];
    if (!app.tcp->recv_all(hdr_buf, lilypad::SIGNAL_HEADER_SIZE)) {
        app.auth_state = AuthState::CONNECTED_UNAUTH;
        app.add_system_msg("Failed to receive login response.");
        return;
    }
    auto header = lilypad::deserialize_header(hdr_buf);
    if (header.type != lilypad::MsgType::AUTH_LOGIN_RESP) {
        app.auth_state = AuthState::CONNECTED_UNAUTH;
        app.add_system_msg("Unexpected response from server.");
        return;
    }

    std::vector<uint8_t> payload(header.payload_len);
    if (!app.tcp->recv_all(payload.data(), header.payload_len)) {
        app.auth_state = AuthState::CONNECTED_UNAUTH;
        app.add_system_msg("Failed to receive login payload.");
        return;
    }

    // Parse: status(1) + client_id(4) + udp_port(2) + token(32) + message\0
    auto status = static_cast<lilypad::AuthStatus>(payload[0]);
    if (status != lilypad::AuthStatus::OK) {
        const char* msg = reinterpret_cast<const char*>(payload.data() + 1 + 4 + 2 + lilypad::SESSION_TOKEN_SIZE);
        app.auth_state = AuthState::CONNECTED_UNAUTH;
        {
            std::lock_guard<std::mutex> lk(app.auth_error_mutex);
            app.auth_error = std::string(msg);
        }
        app.add_system_msg(std::string("Login failed: ") + msg);
        return;
    }

    uint32_t my_id    = lilypad::read_u32(payload.data() + 1);
    uint16_t udp_port = lilypad::read_u16(payload.data() + 5);
    const uint8_t* token = payload.data() + 7;

    // Save session if remember_me
    if (remember_me) {
        save_session(app.server_ip, username, token);
    }

    post_auth_setup(app, my_id, udp_port, token, app.server_ip);
}

void do_register(AppState& app, const std::string& username, const std::string& password) {
    if (app.auth_state.load() != AuthState::CONNECTED_UNAUTH) return;

    app.auth_state = AuthState::REGISTERING;

    auto req = lilypad::make_auth_register_req(username, password);
    {
        std::lock_guard<std::mutex> lk(app.tcp_send_mutex);
        if (!app.tcp || !app.tcp->send_all(req)) {
            app.auth_state = AuthState::CONNECTED_UNAUTH;
            app.add_system_msg("Failed to send register request.");
            return;
        }
    }

    // Read response
    uint8_t hdr_buf[lilypad::SIGNAL_HEADER_SIZE];
    if (!app.tcp->recv_all(hdr_buf, lilypad::SIGNAL_HEADER_SIZE)) {
        app.auth_state = AuthState::CONNECTED_UNAUTH;
        app.add_system_msg("Failed to receive register response.");
        return;
    }
    auto header = lilypad::deserialize_header(hdr_buf);
    if (header.type != lilypad::MsgType::AUTH_REGISTER_RESP) {
        app.auth_state = AuthState::CONNECTED_UNAUTH;
        app.add_system_msg("Unexpected response from server.");
        return;
    }

    std::vector<uint8_t> payload(header.payload_len);
    if (!app.tcp->recv_all(payload.data(), header.payload_len)) {
        app.auth_state = AuthState::CONNECTED_UNAUTH;
        app.add_system_msg("Failed to receive register payload.");
        return;
    }

    // Parse: status(1) + message\0
    auto status = static_cast<lilypad::AuthStatus>(payload[0]);
    const char* msg = reinterpret_cast<const char*>(payload.data() + 1);

    app.auth_state = AuthState::CONNECTED_UNAUTH;

    if (status == lilypad::AuthStatus::OK) {
        app.add_system_msg(std::string("Registration successful: ") + msg);
        {
            std::lock_guard<std::mutex> lk(app.auth_error_mutex);
            app.auth_error.clear();
        }
    } else {
        {
            std::lock_guard<std::mutex> lk(app.auth_error_mutex);
            app.auth_error = std::string(msg);
        }
        app.add_system_msg(std::string("Registration failed: ") + msg);
    }
}

void do_token_login(AppState& app, const std::string& username, const uint8_t* token) {
    if (app.auth_state.load() != AuthState::CONNECTED_UNAUTH) return;

    app.auth_state = AuthState::LOGGING_IN;
    app.my_username = username;

    auto req = lilypad::make_auth_token_login_req(username, token);
    {
        std::lock_guard<std::mutex> lk(app.tcp_send_mutex);
        if (!app.tcp || !app.tcp->send_all(req)) {
            app.auth_state = AuthState::CONNECTED_UNAUTH;
            return;
        }
    }

    // Read response
    uint8_t hdr_buf[lilypad::SIGNAL_HEADER_SIZE];
    if (!app.tcp->recv_all(hdr_buf, lilypad::SIGNAL_HEADER_SIZE)) {
        app.auth_state = AuthState::CONNECTED_UNAUTH;
        return;
    }
    auto header = lilypad::deserialize_header(hdr_buf);
    if (header.type != lilypad::MsgType::AUTH_TOKEN_LOGIN_RESP) {
        app.auth_state = AuthState::CONNECTED_UNAUTH;
        return;
    }

    std::vector<uint8_t> payload(header.payload_len);
    if (!app.tcp->recv_all(payload.data(), header.payload_len)) {
        app.auth_state = AuthState::CONNECTED_UNAUTH;
        return;
    }

    auto status = static_cast<lilypad::AuthStatus>(payload[0]);
    if (status != lilypad::AuthStatus::OK) {
        app.auth_state = AuthState::CONNECTED_UNAUTH;
        // Token expired -- clear saved session, user must re-login
        clear_session(app.server_ip);
        app.add_system_msg("Saved session expired. Please log in.");
        return;
    }

    uint32_t my_id    = lilypad::read_u32(payload.data() + 1);
    uint16_t udp_port = lilypad::read_u16(payload.data() + 5);
    const uint8_t* new_token = payload.data() + 7;

    // Update saved session with new rolling token
    save_session(app.server_ip, username, new_token);

    post_auth_setup(app, my_id, udp_port, new_token, app.server_ip);
}

void do_change_password(AppState& app, const std::string& old_pass, const std::string& new_pass) {
    if (!app.connected) return;

    auto req = lilypad::make_auth_change_pass_req(old_pass, new_pass);
    app.send_tcp(req);
    // Response handled in tcp_receive_thread
}

void do_delete_account(AppState& app, const std::string& password) {
    if (!app.connected) return;

    auto req = lilypad::make_auth_delete_acct_req(password);
    app.send_tcp(req);
    // Response handled in tcp_receive_thread
}

void do_logout(AppState& app) {
    if (!app.connected) return;

    auto msg = lilypad::make_auth_logout_msg();
    app.send_tcp(msg);

    // Clear saved session
    clear_session(app.server_ip);
    app.session_token.clear();

    do_disconnect(app);
}

void do_join_voice(AppState& app, int input_device, int output_device) {
    if (!app.connected || app.in_voice) return;
    try {
        app.capture = std::make_unique<lilypad::AudioCapture>(
            lilypad::SAMPLE_RATE, lilypad::CHANNELS, lilypad::FRAME_SIZE, input_device);
        app.playback = std::make_unique<lilypad::AudioPlayback>(
            lilypad::SAMPLE_RATE, lilypad::CHANNELS, lilypad::FRAME_SIZE, output_device);

        app.in_voice = true;
        app.send_tcp(lilypad::make_voice_join_msg());

        app.send_thread     = std::make_unique<std::thread>(voice_send_thread, std::ref(app));
        app.udp_recv_thread = std::make_unique<std::thread>(udp_receive_thread_func, std::ref(app));
        app.playback_thread = std::make_unique<std::thread>(audio_playback_thread_func, std::ref(app));
    } catch (const std::exception& e) {
        app.in_voice = false;
        app.add_system_msg(std::string("Failed to join voice: ") + e.what());
    }
}

void do_leave_voice(AppState& app) {
    if (!app.in_voice) return;

    app.in_voice = false;
    if (app.connected) {
        app.send_tcp(lilypad::make_voice_leave_msg());
    }

    if (app.udp) app.udp->close();

    if (app.send_thread && app.send_thread->joinable()) app.send_thread->join();
    if (app.udp_recv_thread && app.udp_recv_thread->joinable()) app.udp_recv_thread->join();
    if (app.playback_thread && app.playback_thread->joinable()) app.playback_thread->join();

    app.send_thread.reset();
    app.udp_recv_thread.reset();
    app.playback_thread.reset();
    app.capture.reset();
    app.playback.reset();

    {
        std::lock_guard<std::mutex> lk(app.jitter_mutex);
        app.jitter_buffers.clear();
        app.voice_decoders.clear();
    }
    {
        std::lock_guard<std::mutex> lk(app.voice_activity_mutex);
        app.voice_last_seen.clear();
    }

    if (app.connected) {
        try {
            app.udp = std::make_unique<lilypad::Socket>(lilypad::create_udp_socket());
        } catch (...) {}
    }
}

void do_disconnect(AppState& app) {
    if (!app.connected && app.auth_state.load() == AuthState::DISCONNECTED) return;

    if (app.in_voice) {
        do_leave_voice(app);
    }

    app.screen_sharing = false;
    app.screen_send_cv.notify_all();
    app.screen_decode_cv.notify_all();
    app.watching_user_id = 0;

    if (app.connected) {
        try {
            auto leave = lilypad::make_leave_msg();
            app.send_tcp(leave);
        } catch (...) {}
    }

    app.connected = false;
    app.auth_state = AuthState::DISCONNECTED;

    // Shutdown the raw sockets to unblock any blocking recv/select calls
    // in background threads, but do NOT free TLS yet (threads may still reference it)
    if (app.tcp && app.tcp->valid()) {
        shutdown(app.tcp->get(), SD_BOTH);
    }
    if (app.udp) app.udp->close();

    // Now join all threads (they'll see connected=false and exit)
    if (app.tcp_thread && app.tcp_thread->joinable()) app.tcp_thread->join();
    if (app.screen_thread && app.screen_thread->joinable()) app.screen_thread->join();
    if (app.sys_audio_thread && app.sys_audio_thread->joinable()) app.sys_audio_thread->join();
    if (app.screen_send_thread && app.screen_send_thread->joinable()) app.screen_send_thread->join();
    if (app.screen_decode_thread && app.screen_decode_thread->joinable()) app.screen_decode_thread->join();

    app.tcp_thread.reset();
    app.screen_thread.reset();
    app.sys_audio_thread.reset();
    app.screen_send_thread.reset();
    app.screen_decode_thread.reset();

    // Now safe to close TLS and free everything
    if (app.tcp) app.tcp->close();
    app.tcp.reset();
    app.udp.reset();

    {
        std::lock_guard<std::mutex> lk(app.jitter_mutex);
        app.jitter_buffers.clear();
        app.voice_decoders.clear();
    }
    {
        std::lock_guard<std::mutex> lk(app.voice_activity_mutex);
        app.voice_last_seen.clear();
    }
    {
        std::lock_guard<std::mutex> lk(app.sys_audio_mutex);
        app.sys_audio_frames.clear();
        app.sys_audio_decoder.reset();
    }
    {
        std::lock_guard<std::mutex> lk(app.screen_send_mutex);
        app.screen_send_queue.clear();
    }
    {
        std::lock_guard<std::mutex> lk(app.screen_srv_mutex);
        app.screen_srv = nullptr;
        app.screen_srv_w = 0;
        app.screen_srv_h = 0;
    }
    {
        std::lock_guard<std::mutex> lk(app.screen_frame_mutex);
        app.screen_frame_buf.clear();
        app.screen_frame_flags = 0;
        app.screen_frame_new = false;
    }
    app.force_keyframe = false;
    app.session_token.clear();

    {
        std::lock_guard<std::mutex> lk(app.users_mutex);
        app.users.clear();
    }

    app.add_system_msg("Disconnected.");
}
