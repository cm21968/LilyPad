#include "connection.h"
#include "persistence.h"
#include "network_threads.h"
#include "screen_threads.h"
#include "chat_persistence.h"

#include <fstream>

void do_connect(AppState& app, const std::string& server_ip, const std::string& username) {
    try {
        auto tcp = std::make_unique<lilypad::Socket>(lilypad::create_tcp_socket());

        // Disable Nagle's algorithm for low-latency sends
        int nodelay = 1;
        setsockopt(tcp->get(), IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

        // Large send buffer so screen frame sends don't block the tcp_send_mutex
        int sndbuf = 1024 * 1024; // 1MB
        setsockopt(tcp->get(), SOL_SOCKET, SO_SNDBUF,
                   reinterpret_cast<const char*>(&sndbuf), sizeof(sndbuf));

        // Large receive buffer so TCP window stays open for big H.264 frames
        int rcvbuf = 1024 * 1024; // 1MB
        setsockopt(tcp->get(), SOL_SOCKET, SO_RCVBUF,
                   reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));

        sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_port   = htons(7777);
        inet_pton(AF_INET, server_ip.c_str(), &server_addr.sin_addr);

        if (connect(tcp->get(), reinterpret_cast<sockaddr*>(&server_addr),
                    sizeof(server_addr)) == SOCKET_ERROR) {
            app.add_system_msg("Failed to connect: error " + std::to_string(WSAGetLastError()));
            return;
        }

        auto join_msg = lilypad::make_join_msg(username);
        tcp->send_all(join_msg);

        uint8_t hdr_buf[lilypad::SIGNAL_HEADER_SIZE];
        if (!tcp->recv_all(hdr_buf, lilypad::SIGNAL_HEADER_SIZE)) {
            app.add_system_msg("Failed to receive welcome.");
            return;
        }
        auto welcome_hdr = lilypad::deserialize_header(hdr_buf);
        if (welcome_hdr.type != lilypad::MsgType::WELCOME) {
            app.add_system_msg("Unexpected response from server.");
            return;
        }

        std::vector<uint8_t> welcome_payload(welcome_hdr.payload_len);
        if (!tcp->recv_all(welcome_payload.data(), welcome_hdr.payload_len)) {
            app.add_system_msg("Failed to receive welcome payload.");
            return;
        }

        uint32_t my_id    = lilypad::read_u32(welcome_payload.data());
        uint16_t udp_port = lilypad::read_u16(welcome_payload.data() + 4);

        auto udp = std::make_unique<lilypad::Socket>(lilypad::create_udp_socket());

        sockaddr_in udp_dest{};
        udp_dest.sin_family = AF_INET;
        udp_dest.sin_port   = htons(udp_port);
        inet_pton(AF_INET, server_ip.c_str(), &udp_dest.sin_addr);

        app.tcp      = std::move(tcp);
        app.udp      = std::move(udp);
        app.udp_dest = udp_dest;
        app.my_id    = my_id;
        app.my_username = username;
        app.server_ip = server_ip;
        app.in_voice = false;

        {
            std::lock_guard<std::mutex> lk(app.users_mutex);
            app.users.clear();
        }
        {
            std::lock_guard<std::mutex> lk(app.volume_mutex);
            app.user_volumes.clear();
        }

        // Reset jitter buffers
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

        app.connected = true;
        app.add_system_msg("Connected! Your ID: " + std::to_string(my_id));

        // Send CHAT_SYNC to get messages newer than our cache
        auto sync_msg = lilypad::make_chat_sync_msg(app.last_known_seq.load());
        app.send_tcp(sync_msg);

        // Start TCP receive and screen decode threads only (voice threads start on Join Voice)
        app.tcp_thread = std::make_unique<std::thread>(tcp_receive_thread, std::ref(app));
        app.screen_decode_thread = std::make_unique<std::thread>(screen_decode_thread_func, std::ref(app));

    } catch (const std::exception& e) {
        app.add_system_msg(std::string("Connection error: ") + e.what());
    }
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

    // Close UDP to unblock recv thread, then join voice threads
    if (app.udp) app.udp->close();

    if (app.send_thread && app.send_thread->joinable()) app.send_thread->join();
    if (app.udp_recv_thread && app.udp_recv_thread->joinable()) app.udp_recv_thread->join();
    if (app.playback_thread && app.playback_thread->joinable()) app.playback_thread->join();

    app.send_thread.reset();
    app.udp_recv_thread.reset();
    app.playback_thread.reset();
    app.capture.reset();
    app.playback.reset();

    // Clear jitter buffers
    {
        std::lock_guard<std::mutex> lk(app.jitter_mutex);
        app.jitter_buffers.clear();
        app.voice_decoders.clear();
    }
    {
        std::lock_guard<std::mutex> lk(app.voice_activity_mutex);
        app.voice_last_seen.clear();
    }

    // Recreate UDP socket for potential rejoin
    if (app.connected) {
        try {
            app.udp = std::make_unique<lilypad::Socket>(lilypad::create_udp_socket());
        } catch (...) {}
    }
}

void do_disconnect(AppState& app) {
    if (!app.connected) return;

    // Leave voice if active
    if (app.in_voice) {
        do_leave_voice(app);
    }

    // Stop screen sharing if active
    app.screen_sharing = false;
    app.screen_send_cv.notify_all();   // wake send thread to exit
    app.screen_decode_cv.notify_all(); // wake decode thread to exit
    app.watching_user_id = 0;

    try {
        auto leave = lilypad::make_leave_msg();
        app.send_tcp(leave);
    } catch (...) {}

    app.connected = false;

    if (app.tcp) app.tcp->close();
    if (app.udp) app.udp->close();

    if (app.tcp_thread  && app.tcp_thread->joinable())  app.tcp_thread->join();
    if (app.screen_thread && app.screen_thread->joinable()) app.screen_thread->join();
    if (app.sys_audio_thread && app.sys_audio_thread->joinable()) app.sys_audio_thread->join();
    if (app.screen_send_thread && app.screen_send_thread->joinable()) app.screen_send_thread->join();
    if (app.screen_decode_thread && app.screen_decode_thread->joinable()) app.screen_decode_thread->join();

    app.tcp_thread.reset();
    app.screen_thread.reset();
    app.sys_audio_thread.reset();
    app.screen_send_thread.reset();
    app.screen_decode_thread.reset();
    app.tcp.reset();
    app.udp.reset();

    // Clean up jitter buffers and voice decoders
    {
        std::lock_guard<std::mutex> lk(app.jitter_mutex);
        app.jitter_buffers.clear();
        app.voice_decoders.clear();
    }
    {
        std::lock_guard<std::mutex> lk(app.voice_activity_mutex);
        app.voice_last_seen.clear();
    }

    // Clean up system audio state
    {
        std::lock_guard<std::mutex> lk(app.sys_audio_mutex);
        app.sys_audio_frames.clear();
        app.sys_audio_decoder.reset();
    }

    // Clean up screen send queue
    {
        std::lock_guard<std::mutex> lk(app.screen_send_mutex);
        app.screen_send_queue.clear();
    }

    // Clean up screen viewer state (SRV is owned by decoder thread, not us)
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

    {
        std::lock_guard<std::mutex> lk(app.users_mutex);
        app.users.clear();
    }

    app.add_system_msg("Disconnected.");
}
