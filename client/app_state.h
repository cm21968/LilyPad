#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "audio.h"
#include "audio_codec.h"
#include "network.h"
#include "protocol.h"
#include "tls_socket.h"

#include <d3d11.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// ── App version (compared against server's update notification) ──
static constexpr const char* APP_VERSION = "1.0.7";

// ── GitHub update check URL (raw version.txt: line 1 = version, line 2 = download URL) ──
static constexpr const char* UPDATE_CHECK_URL = "https://raw.githubusercontent.com/cm21968/LilyPad/main/version.txt";

// ── Semver comparison: returns true if remote is strictly newer than local ──
inline bool is_newer_version(const char* local, const std::string& remote) {
    int l_major = 0, l_minor = 0, l_patch = 0;
    int r_major = 0, r_minor = 0, r_patch = 0;
    (void)sscanf(local, "%d.%d.%d", &l_major, &l_minor, &l_patch);
    (void)sscanf(remote.c_str(), "%d.%d.%d", &r_major, &r_minor, &r_patch);
    if (r_major != l_major) return r_major > l_major;
    if (r_minor != l_minor) return r_minor > l_minor;
    return r_patch > l_patch;
}

// ════════════════════════════════════════════════════════════════
//  Authentication state
// ════════════════════════════════════════════════════════════════
enum class AuthState {
    DISCONNECTED,
    CONNECTED_UNAUTH,  // TLS connected, not yet authenticated
    LOGGING_IN,
    REGISTERING,
    AUTHENTICATED,
};

// ════════════════════════════════════════════════════════════════
//  Server favorites
// ════════════════════════════════════════════════════════════════
struct ServerFavorite {
    std::string name;      // display name
    std::string ip;        // server IP/hostname
    std::string username;  // last-used username for this server
};

// ════════════════════════════════════════════════════════════════
//  Settings (auto-connect, etc.)
// ════════════════════════════════════════════════════════════════
struct AppSettings {
    bool auto_connect = false;
    std::string last_server_ip;
    std::string last_username;
};

// ════════════════════════════════════════════════════════════════
//  Application state structs
// ════════════════════════════════════════════════════════════════
struct UserEntry {
    uint32_t    id;
    std::string name;
    bool        is_sharing = false;
    bool        in_voice = false;
};

struct ChatMessage {
    uint32_t    sender_id;
    std::string sender_name;
    std::string text;
    bool        is_system;
    uint64_t    seq = 0;
    int64_t     timestamp = 0;
};

// Per-user jitter buffer for voice reception
struct JitterBuffer {
    std::deque<std::vector<float>> frames;
    bool primed = false;  // true once pre-buffer threshold reached
    static constexpr size_t MAX_DEPTH  = 4;  // 80ms max buffering
    static constexpr size_t PRE_BUFFER = 2;  // 40ms pre-buffer before playback starts
};

// Item in the screen-share send queue (video frames + system audio)
struct ScreenSendItem {
    std::vector<uint8_t> data;    // fully formed TCP message
    bool                 is_audio; // true = SCREEN_AUDIO, false = SCREEN_FRAME
};

// ════════════════════════════════════════════════════════════════
//  Main application state
// ════════════════════════════════════════════════════════════════
struct AppState {
    // Connection
    std::atomic<bool> connected{false};
    std::atomic<bool> running{true};
    uint32_t          my_id = 0;
    std::string       my_username;
    std::string       server_ip;

    // Authentication
    std::atomic<AuthState> auth_state{AuthState::DISCONNECTED};
    std::vector<uint8_t>   session_token; // 32 bytes, raw
    std::mutex             auth_error_mutex;
    std::string            auth_error;
    bool                   trust_self_signed = false;

    // Voice channel (separate from text chat)
    std::atomic<bool> in_voice{false};

    // Update notification (from server or background GitHub check)
    std::mutex              update_mutex;
    std::string             update_version;
    std::string             update_url;
    std::atomic<bool>       update_available{false};

    // Network handles
    std::unique_ptr<lilypad::TlsSocket> tcp;
    std::unique_ptr<lilypad::Socket>    udp;
    sockaddr_in                         udp_dest{};

    // User list
    std::mutex              users_mutex;
    std::vector<UserEntry>  users;

    // Chat messages
    std::mutex              chat_mutex;
    std::vector<ChatMessage> chat_messages;
    std::atomic<uint64_t>   last_known_seq{0};

    // Per-user volume (client_id -> volume 0.0-2.0, default 1.0)
    std::mutex                           volume_mutex;
    std::unordered_map<uint32_t, float>  user_volumes;

    // Push-to-talk
    std::atomic<bool> ptt_enabled{false};
    std::atomic<int>  ptt_key{0x56};        // default: V key
    std::atomic<bool> ptt_active{false};     // true when key is held

    // Mute
    std::atomic<bool> muted{false};

    // Noise suppression (RNNoise)
    std::atomic<bool> noise_suppression{false};

    // Audio handles
    std::unique_ptr<lilypad::AudioCapture>  capture;
    std::unique_ptr<lilypad::AudioPlayback> playback;

    // TCP send mutex (TCP thread reads, UI thread sends chat)
    std::mutex tcp_send_mutex;

    // Jitter buffers for voice reception (per-user)
    std::mutex                                              jitter_mutex;
    std::unordered_map<uint32_t, JitterBuffer>              jitter_buffers;
    std::unordered_map<uint32_t, lilypad::OpusDecoderWrapper> voice_decoders;

    // Voice activity tracking (per-user, timestamp of last received voice packet)
    std::mutex                                                             voice_activity_mutex;
    std::unordered_map<uint32_t, std::chrono::steady_clock::time_point>    voice_last_seen;

    // Threads
    std::unique_ptr<std::thread> tcp_thread;
    std::unique_ptr<std::thread> send_thread;
    std::unique_ptr<std::thread> udp_recv_thread;
    std::unique_ptr<std::thread> playback_thread;
    std::unique_ptr<std::thread> screen_thread;
    std::unique_ptr<std::thread> sys_audio_thread;

    // System audio playback (received from screen sharer)
    std::mutex                             sys_audio_mutex;
    std::deque<std::vector<float>>         sys_audio_frames;
    std::unique_ptr<lilypad::OpusDecoderWrapper> sys_audio_decoder;
    float                                  stream_volume = 1.0f;  // 0.0-1.0

    // Screen send queue -- decouples capture/encode from TCP sending
    std::mutex                          screen_send_mutex;
    std::condition_variable             screen_send_cv;
    std::deque<ScreenSendItem>          screen_send_queue;
    std::unique_ptr<std::thread>        screen_send_thread;

    // Screen sharing -- outgoing (this client is sharing)
    std::atomic<bool> screen_sharing{false};

    // Screen sharing -- incoming (watching another client)
    std::atomic<uint32_t> watching_user_id{0};      // 0 = not watching
    std::mutex            screen_frame_mutex;
    std::vector<uint8_t>  screen_frame_buf;          // H.264 data from network
    uint8_t               screen_frame_flags = 0;    // flags byte from protocol
    bool                  screen_frame_new   = false;

    // H.264 decode thread (produces SRV directly via decoder)
    std::condition_variable screen_decode_cv;
    std::unique_ptr<std::thread> screen_decode_thread;

    // H.264 decoder output SRV (set by decode thread, read by main thread)
    std::mutex                    screen_srv_mutex;
    ID3D11ShaderResourceView*     screen_srv       = nullptr;  // not owned -- decoder owns it
    int                           screen_srv_w     = 0;
    int                           screen_srv_h     = 0;

    // Keyframe request from server
    std::atomic<bool> force_keyframe{false};
    std::atomic<int>  h264_bitrate{0};  // 0 = auto (set based on resolution)

    void add_system_msg(const std::string& text) {
        std::lock_guard<std::mutex> lk(chat_mutex);
        chat_messages.push_back({0, "", text, true, 0, 0});
        if (chat_messages.size() > 5000)
            chat_messages.erase(chat_messages.begin());
    }

    void add_chat_msg(uint32_t sender_id, const std::string& name, const std::string& text,
                      uint64_t seq = 0, int64_t timestamp = 0) {
        std::lock_guard<std::mutex> lk(chat_mutex);
        chat_messages.push_back({sender_id, name, text, false, seq, timestamp});
        if (chat_messages.size() > 5000)
            chat_messages.erase(chat_messages.begin());
    }

    float get_volume(uint32_t client_id) {
        std::lock_guard<std::mutex> lk(volume_mutex);
        auto it = user_volumes.find(client_id);
        if (it != user_volumes.end()) return it->second;
        return 1.0f;
    }

    void set_volume(uint32_t client_id, float vol) {
        std::lock_guard<std::mutex> lk(volume_mutex);
        user_volumes[client_id] = vol;
    }

    void send_tcp(const std::vector<uint8_t>& data) {
        std::lock_guard<std::mutex> lk(tcp_send_mutex);
        if (tcp && tcp->valid()) tcp->send_all(data);
    }

    std::string lookup_username(uint32_t uid) {
        std::lock_guard<std::mutex> lk(users_mutex);
        for (auto& u : users)
            if (u.id == uid) return u.name;
        return "User #" + std::to_string(uid);
    }
};

// ════════════════════════════════════════════════════════════════
//  PTT key names
// ════════════════════════════════════════════════════════════════
struct PttKeyOption {
    int         vk;
    const char* name;
};

inline const PttKeyOption g_ptt_keys[] = {
    { 0x56, "V" },
    { 0x42, "B" },
    { 0x47, "G" },
    { 0x54, "T" },
    { VK_CAPITAL, "Caps Lock" },
    { VK_XBUTTON1, "Mouse 4" },
    { VK_XBUTTON2, "Mouse 5" },
};
inline const int g_ptt_key_count = sizeof(g_ptt_keys) / sizeof(g_ptt_keys[0]);
