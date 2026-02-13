#include "network_threads.h"
#include "connection.h"
#include "persistence.h"
#include "chat_persistence.h"

#include <rnnoise.h>

#include <algorithm>
#include <fstream>

void tcp_receive_thread(AppState& app) {
    while (app.running && app.connected) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(app.tcp->get(), &read_set);

        timeval timeout{};
        timeout.tv_sec  = 0;
        timeout.tv_usec = 200000;

        int ready = select(0, &read_set, nullptr, nullptr, &timeout);
        if (ready <= 0) continue;

        uint8_t hdr[lilypad::SIGNAL_HEADER_SIZE];
        if (!app.tcp->recv_all(hdr, lilypad::SIGNAL_HEADER_SIZE)) {
            app.add_system_msg("Disconnected from server.");
            app.connected = false;
            break;
        }

        auto header = lilypad::deserialize_header(hdr);

        std::vector<uint8_t> payload;
        if (header.payload_len > 0) {
            payload.resize(header.payload_len);
            if (!app.tcp->recv_all(payload.data(), header.payload_len)) {
                app.connected = false;
                break;
            }
        }

        switch (header.type) {
        case lilypad::MsgType::USER_JOINED: {
            uint32_t uid = lilypad::read_u32(payload.data());
            std::string name(reinterpret_cast<const char*>(payload.data() + 4));
            {
                std::lock_guard<std::mutex> lk(app.users_mutex);
                app.users.push_back({uid, name});
            }
            app.add_system_msg(name + " joined.");
            break;
        }
        case lilypad::MsgType::USER_LEFT: {
            uint32_t uid = lilypad::read_u32(payload.data());
            std::string name;
            {
                std::lock_guard<std::mutex> lk(app.users_mutex);
                auto it = std::find_if(app.users.begin(), app.users.end(),
                    [uid](const UserEntry& u) { return u.id == uid; });
                if (it != app.users.end()) {
                    name = it->name;
                    app.users.erase(it);
                }
            }
            {
                std::lock_guard<std::mutex> lk(app.volume_mutex);
                app.user_volumes.erase(uid);
            }
            // If we were watching this user, stop
            if (app.watching_user_id.load() == uid) {
                app.watching_user_id = 0;
            }
            app.add_system_msg((name.empty() ? "User #" + std::to_string(uid) : name) + " left.");
            break;
        }
        case lilypad::MsgType::TEXT_CHAT: {
            // v2 format: seq(8) + client_id(4) + timestamp(8) + sender_name\0 + text\0
            if (payload.size() > 20) {
                uint64_t seq = lilypad::read_u64(payload.data());
                uint32_t uid = lilypad::read_u32(payload.data() + 8);
                int64_t ts   = static_cast<int64_t>(lilypad::read_u64(payload.data() + 12));
                // Find sender_name (null-terminated starting at offset 20)
                const char* p = reinterpret_cast<const char*>(payload.data() + 20);
                std::string sender_name(p);
                size_t text_offset = 20 + sender_name.size() + 1;
                std::string text;
                if (text_offset < payload.size()) {
                    text = std::string(reinterpret_cast<const char*>(payload.data() + text_offset));
                }
                // Skip if already in cache
                if (seq <= app.last_known_seq.load()) break;
                app.add_chat_msg(uid, sender_name, text, seq, ts);
                app.last_known_seq = seq;
                // Append to local cache
                std::string cache_path = get_chat_cache_path(app.server_ip);
                if (!cache_path.empty()) {
                    std::ofstream cf(cache_path, std::ios::app);
                    if (cf.is_open()) {
                        cf << lilypad::serialize_chat_line(seq, sender_name, ts, text) << '\n';
                    }
                }
            }
            break;
        }
        case lilypad::MsgType::VOICE_JOINED: {
            if (payload.size() >= 4) {
                uint32_t uid = lilypad::read_u32(payload.data());
                std::lock_guard<std::mutex> lk(app.users_mutex);
                for (auto& u : app.users) {
                    if (u.id == uid) { u.in_voice = true; break; }
                }
            }
            break;
        }
        case lilypad::MsgType::VOICE_LEFT: {
            if (payload.size() >= 4) {
                uint32_t uid = lilypad::read_u32(payload.data());
                std::lock_guard<std::mutex> lk(app.users_mutex);
                for (auto& u : app.users) {
                    if (u.id == uid) { u.in_voice = false; break; }
                }
            }
            break;
        }
        case lilypad::MsgType::SCREEN_START: {
            if (payload.size() >= 4) {
                uint32_t uid = lilypad::read_u32(payload.data());
                std::lock_guard<std::mutex> lk(app.users_mutex);
                for (auto& u : app.users) {
                    if (u.id == uid) { u.is_sharing = true; break; }
                }
            }
            break;
        }
        case lilypad::MsgType::SCREEN_STOP: {
            if (payload.size() >= 4) {
                uint32_t uid = lilypad::read_u32(payload.data());
                {
                    std::lock_guard<std::mutex> lk(app.users_mutex);
                    for (auto& u : app.users) {
                        if (u.id == uid) { u.is_sharing = false; break; }
                    }
                }
                // If we were watching this user, stop
                if (app.watching_user_id.load() == uid) {
                    app.watching_user_id = 0;
                }
            }
            break;
        }
        case lilypad::MsgType::SCREEN_FRAME: {
            // Server relay: sharer_id(4) + width(2) + height(2) + flags(1) + h264_data
            if (payload.size() >= 9) {
                uint32_t sharer_id = lilypad::read_u32(payload.data());
                if (sharer_id == app.watching_user_id.load()) {
                    uint8_t flags = payload[8];
                    const uint8_t* frame_data = payload.data() + 9;
                    size_t frame_len = payload.size() - 9;

                    {
                        std::lock_guard<std::mutex> lk(app.screen_frame_mutex);
                        app.screen_frame_buf.assign(frame_data, frame_data + frame_len);
                        app.screen_frame_flags = flags;
                        app.screen_frame_new = true;
                    }
                    app.screen_decode_cv.notify_one();
                }
            }
            break;
        }
        case lilypad::MsgType::SCREEN_AUDIO: {
            // Server relay: sharer_id(4) + opus_data
            if (payload.size() > 4) {
                uint32_t sharer_id = lilypad::read_u32(payload.data());
                if (sharer_id == app.watching_user_id.load()) {
                    const uint8_t* opus_data = payload.data() + 4;
                    int opus_len = static_cast<int>(payload.size() - 4);

                    std::lock_guard<std::mutex> lk(app.sys_audio_mutex);
                    // Create decoder on first use
                    if (!app.sys_audio_decoder) {
                        app.sys_audio_decoder = std::make_unique<lilypad::OpusDecoderWrapper>();
                    }
                    try {
                        auto pcm = app.sys_audio_decoder->decode(opus_data, opus_len);
                        app.sys_audio_frames.push_back(std::move(pcm));
                        // Limit buffer depth to prevent unbounded growth
                        while (app.sys_audio_frames.size() > 8) {
                            app.sys_audio_frames.pop_front();
                        }
                    } catch (...) {}
                }
            }
            break;
        }
        case lilypad::MsgType::SCREEN_REQUEST_KEYFRAME: {
            // Server requests that we produce an IDR keyframe
            app.force_keyframe = true;
            break;
        }
        case lilypad::MsgType::UPDATE_AVAILABLE: {
            // Server sends: version\0url\0
            if (payload.size() >= 3) {
                const char* p = reinterpret_cast<const char*>(payload.data());
                std::string version(p);
                size_t url_offset = version.size() + 1;
                if (url_offset < payload.size()) {
                    std::string url(p + url_offset);
                    if (!version.empty() && !url.empty() && is_newer_version(APP_VERSION, version)) {
                        std::lock_guard<std::mutex> lk(app.update_mutex);
                        app.update_version = version;
                        app.update_url     = url;
                        app.update_available = true;
                    }
                }
            }
            break;
        }
        case lilypad::MsgType::AUTH_CHANGE_PASS_RESP: {
            if (payload.size() >= 2) {
                auto status = static_cast<lilypad::AuthStatus>(payload[0]);
                const char* msg = reinterpret_cast<const char*>(payload.data() + 1);
                if (status == lilypad::AuthStatus::OK) {
                    app.add_system_msg(std::string("Password changed: ") + msg);
                    // Clear saved session since all sessions were invalidated
                    clear_session(app.server_ip);
                } else {
                    app.add_system_msg(std::string("Password change failed: ") + msg);
                }
                {
                    std::lock_guard<std::mutex> lk(app.auth_error_mutex);
                    app.auth_error = (status == lilypad::AuthStatus::OK) ? "" : std::string(msg);
                }
            }
            break;
        }
        case lilypad::MsgType::AUTH_DELETE_ACCT_RESP: {
            if (payload.size() >= 2) {
                auto status = static_cast<lilypad::AuthStatus>(payload[0]);
                const char* msg = reinterpret_cast<const char*>(payload.data() + 1);
                if (status == lilypad::AuthStatus::OK) {
                    app.add_system_msg("Account deleted.");
                    clear_session(app.server_ip);
                    app.connected = false;
                } else {
                    app.add_system_msg(std::string("Delete account failed: ") + msg);
                }
                {
                    std::lock_guard<std::mutex> lk(app.auth_error_mutex);
                    app.auth_error = (status == lilypad::AuthStatus::OK) ? "" : std::string(msg);
                }
            }
            break;
        }
        default:
            break;
        }
    }
}

void voice_send_thread(AppState& app) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    lilypad::OpusEncoderWrapper encoder;
    uint32_t sequence = 0;

    // RNNoise state -- created once, reused for the lifetime of the thread
    DenoiseState* rnn_st = rnnoise_create(nullptr);
    const int rnn_frame = 480; // rnnoise processes 480 samples at a time

    while (app.running && app.connected && app.in_voice) {
        try {
            auto pcm = app.capture->read_frame();

            // Mute or push-to-talk: determine if we should transmit
            bool should_transmit = !app.muted.load();
            if (should_transmit && app.ptt_enabled) {
                should_transmit = app.ptt_active.load();
            }

            if (!should_transmit) {
                // Still read from mic to keep stream flowing, but don't send
                continue;
            }

            // Apply RNNoise noise suppression if enabled
            if (app.noise_suppression.load()) {
                // RNNoise expects float samples in [-32768, 32768] range
                // Our PCM is in [-1.0, 1.0] (PortAudio float format)
                // Process two 480-sample sub-frames (our frame is 960 samples)
                std::vector<float> rnn_buf(rnn_frame);
                for (int sub = 0; sub < 2; ++sub) {
                    // Scale up to rnnoise range
                    for (int i = 0; i < rnn_frame; ++i) {
                        rnn_buf[i] = pcm[sub * rnn_frame + i] * 32768.0f;
                    }
                    rnnoise_process_frame(rnn_st, rnn_buf.data(), rnn_buf.data());
                    // Scale back to [-1.0, 1.0]
                    for (int i = 0; i < rnn_frame; ++i) {
                        pcm[sub * rnn_frame + i] = rnn_buf[i] / 32768.0f;
                    }
                }
            }

            auto opus_data = encoder.encode(pcm.data());

            lilypad::VoicePacket pkt;
            pkt.client_id = app.my_id;
            pkt.sequence  = sequence++;
            pkt.opus_data = std::move(opus_data);

            auto bytes = pkt.to_bytes();
            sendto(app.udp->get(), reinterpret_cast<const char*>(bytes.data()),
                   static_cast<int>(bytes.size()), 0,
                   reinterpret_cast<const sockaddr*>(&app.udp_dest),
                   sizeof(app.udp_dest));
        } catch (...) {
            break;
        }
    }

    if (rnn_st) rnnoise_destroy(rnn_st);
}

// ── UDP receive thread: reads UDP packets, decodes Opus, pushes into per-user jitter buffers ──
void udp_receive_thread_func(AppState& app) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    uint8_t buf[lilypad::MAX_VOICE_PACKET];

    while (app.running && app.connected && app.in_voice) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(app.udp->get(), &read_set);

        timeval timeout{};
        timeout.tv_sec  = 0;
        timeout.tv_usec = 20000; // 20ms

        int ready = select(0, &read_set, nullptr, nullptr, &timeout);
        if (ready <= 0) continue;

        // Read all available UDP packets
        while (true) {
            sockaddr_in sender{};
            int sender_len = sizeof(sender);
            int received = recvfrom(app.udp->get(), reinterpret_cast<char*>(buf), sizeof(buf), 0,
                                    reinterpret_cast<sockaddr*>(&sender), &sender_len);
            if (received < static_cast<int>(lilypad::VOICE_HEADER_SIZE)) break;

            auto pkt = lilypad::VoicePacket::from_bytes(buf, static_cast<size_t>(received));

            // Record voice activity for talking indicator
            {
                std::lock_guard<std::mutex> lk(app.voice_activity_mutex);
                app.voice_last_seen[pkt.client_id] = std::chrono::steady_clock::now();
            }

            std::lock_guard<std::mutex> lk(app.jitter_mutex);
            // Get or create decoder for this user
            auto [dec_it, dec_inserted] = app.voice_decoders.try_emplace(pkt.client_id);
            auto pcm = dec_it->second.decode(pkt.opus_data.data(),
                                              static_cast<int>(pkt.opus_data.size()));

            // Push into jitter buffer
            auto& jb = app.jitter_buffers[pkt.client_id];
            jb.frames.push_back(std::move(pcm));
            // Discard oldest if exceeded max depth to prevent latency buildup
            while (jb.frames.size() > JitterBuffer::MAX_DEPTH) {
                jb.frames.pop_front();
            }

            // Check if more data is available
            fd_set peek_set;
            FD_ZERO(&peek_set);
            FD_SET(app.udp->get(), &peek_set);
            timeval zero_tv{0, 0};
            if (select(0, &peek_set, nullptr, nullptr, &zero_tv) <= 0) break;
        }
    }
}

// ── Audio playback thread: hardware-paced, drains jitter buffers, mixes, writes ──
void audio_playback_thread_func(AppState& app) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    const size_t frame_len = lilypad::FRAME_SIZE * lilypad::CHANNELS;

    while (app.running && app.connected && app.in_voice) {
        std::vector<float> mixed(frame_len, 0.0f);

        {
            std::lock_guard<std::mutex> lk(app.jitter_mutex);

            // Collect IDs to remove stale buffers later
            std::vector<uint32_t> empty_ids;

            for (auto& [uid, jb] : app.jitter_buffers) {
                // Pre-buffer: wait until we have enough frames before starting playback
                if (!jb.primed) {
                    if (jb.frames.size() >= JitterBuffer::PRE_BUFFER) {
                        jb.primed = true;
                    } else {
                        continue; // still buffering
                    }
                }

                std::vector<float> pcm;
                if (!jb.frames.empty()) {
                    pcm = std::move(jb.frames.front());
                    jb.frames.pop_front();
                } else {
                    // Buffer underrun -- use Opus PLC for interpolated audio
                    auto dec_it = app.voice_decoders.find(uid);
                    if (dec_it != app.voice_decoders.end()) {
                        pcm = dec_it->second.decode_plc();
                    }
                    // Reset primed so we re-buffer on next burst of packets
                    jb.primed = false;
                }

                if (pcm.empty()) continue;

                float vol = app.get_volume(uid);
                for (size_t i = 0; i < frame_len && i < pcm.size(); ++i) {
                    mixed[i] += pcm[i] * vol;
                }
            }
        }

        // Mix in system audio from screen sharing (if any)
        {
            std::lock_guard<std::mutex> lk(app.sys_audio_mutex);
            if (!app.sys_audio_frames.empty()) {
                float vol = app.stream_volume;
                auto& sa = app.sys_audio_frames.front();
                for (size_t i = 0; i < frame_len && i < sa.size(); ++i) {
                    mixed[i] += sa[i] * vol;
                }
                app.sys_audio_frames.pop_front();
            }
        }

        // Clamp
        for (float& s : mixed) {
            s = std::clamp(s, -1.0f, 1.0f);
        }

        try {
            // Blocking write -- paces this thread at hardware rate (~20ms)
            app.playback->write_frame(mixed);
        } catch (...) { break; }
    }
}
