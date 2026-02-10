#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "audio.h"
#include "audio_codec.h"
#include "network.h"
#include "protocol.h"
#include "screen_capture.h"
#include "system_audio.h"

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

#include <d3d11.h>
#include <dxgi.h>
#include <windows.h>
#include <tchar.h>

#include <rnnoise.h>

#include <shellapi.h>
#include <shlobj.h>
#include <wininet.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// ── App version (compared against server's update notification) ──
static constexpr const char* APP_VERSION = "1.0.0";

// ── GitHub update check URL (raw version.txt: line 1 = version, line 2 = download URL) ──
static constexpr const char* UPDATE_CHECK_URL = "https://raw.githubusercontent.com/cm21968/LilyPad/main/version.txt";

// ── Semver comparison: returns true if remote is strictly newer than local ──
static bool is_newer_version(const char* local, const std::string& remote) {
    int l_major = 0, l_minor = 0, l_patch = 0;
    int r_major = 0, r_minor = 0, r_patch = 0;
    sscanf(local, "%d.%d.%d", &l_major, &l_minor, &l_patch);
    sscanf(remote.c_str(), "%d.%d.%d", &r_major, &r_minor, &r_patch);
    if (r_major != l_major) return r_major > l_major;
    if (r_minor != l_minor) return r_minor > l_minor;
    return r_patch > l_patch;
}

// ── Forward declarations ──
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// ════════════════════════════════════════════════════════════════
//  Server favorites
// ════════════════════════════════════════════════════════════════
struct ServerFavorite {
    std::string name;      // display name
    std::string ip;        // server IP/hostname
    std::string username;  // last-used username for this server
};

static std::string get_favorites_path() {
    PWSTR documents = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents))) {
        // Convert wide string to UTF-8
        int len = WideCharToMultiByte(CP_UTF8, 0, documents, -1, nullptr, 0, nullptr, nullptr);
        std::string path(len - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, documents, -1, path.data(), len, nullptr, nullptr);
        CoTaskMemFree(documents);

        path += "\\LilyPad";
        CreateDirectoryA(path.c_str(), nullptr); // create if doesn't exist
        return path + "\\favorites.txt";
    }
    return "";
}

// File format: one server per line, tab-separated: name\tip\tusername
static std::vector<ServerFavorite> load_favorites() {
    std::vector<ServerFavorite> favs;
    std::string path = get_favorites_path();
    if (path.empty()) return favs;

    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        auto tab1 = line.find('\t');
        if (tab1 != std::string::npos) {
            auto tab2 = line.find('\t', tab1 + 1);
            std::string name = line.substr(0, tab1);
            std::string ip, user;
            if (tab2 != std::string::npos) {
                ip   = line.substr(tab1 + 1, tab2 - tab1 - 1);
                user = line.substr(tab2 + 1);
            } else {
                ip = line.substr(tab1 + 1);
            }
            favs.push_back({name, ip, user});
        } else {
            favs.push_back({line, line, ""});
        }
    }
    return favs;
}

static void save_favorites(const std::vector<ServerFavorite>& favs) {
    std::string path = get_favorites_path();
    if (path.empty()) return;

    std::ofstream file(path, std::ios::trunc);
    for (auto& f : favs) {
        file << f.name << '\t' << f.ip << '\t' << f.username << '\n';
    }
}

// ════════════════════════════════════════════════════════════════
//  D3D11 globals
// ════════════════════════════════════════════════════════════════
static ID3D11Device*           g_d3d_device    = nullptr;
static ID3D11DeviceContext*    g_d3d_context   = nullptr;
static IDXGISwapChain*         g_swap_chain    = nullptr;
static ID3D11RenderTargetView* g_rtv           = nullptr;

static bool CreateD3DDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount        = 2;
    sd.BufferDesc.Width   = 0;
    sd.BufferDesc.Height  = 0;
    sd.BufferDesc.Format  = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator   = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags              = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow       = hwnd;
    sd.SampleDesc.Count   = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed           = TRUE;
    sd.SwapEffect         = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL level;
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        levels, 1, D3D11_SDK_VERSION, &sd,
        &g_swap_chain, &g_d3d_device, &level, &g_d3d_context);

    if (FAILED(hr)) return false;

    ID3D11Texture2D* back_buffer = nullptr;
    g_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    g_d3d_device->CreateRenderTargetView(back_buffer, nullptr, &g_rtv);
    back_buffer->Release();
    return true;
}

static void CleanupD3D() {
    if (g_rtv)        { g_rtv->Release();        g_rtv = nullptr; }
    if (g_swap_chain) { g_swap_chain->Release();  g_swap_chain = nullptr; }
    if (g_d3d_context){ g_d3d_context->Release(); g_d3d_context = nullptr; }
    if (g_d3d_device) { g_d3d_device->Release();  g_d3d_device = nullptr; }
}

static void ResizeD3D(UINT width, UINT height) {
    if (!g_d3d_device || width == 0 || height == 0) return;
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    g_swap_chain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    ID3D11Texture2D* back_buffer = nullptr;
    g_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    g_d3d_device->CreateRenderTargetView(back_buffer, nullptr, &g_rtv);
    back_buffer->Release();
}

static LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED)
            ResizeD3D(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ════════════════════════════════════════════════════════════════
//  Application state
// ════════════════════════════════════════════════════════════════
struct UserEntry {
    uint32_t    id;
    std::string name;
    bool        is_sharing = false;
};

struct ChatMessage {
    uint32_t    sender_id;
    std::string sender_name;
    std::string text;
    bool        is_system;
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

struct AppState {
    // Connection
    std::atomic<bool> connected{false};
    std::atomic<bool> running{true};
    uint32_t          my_id = 0;
    std::string       my_username;

    // Update notification (from server or background GitHub check)
    std::mutex              update_mutex;
    std::string             update_version;
    std::string             update_url;
    std::atomic<bool>       update_available{false};

    // Network handles
    std::unique_ptr<lilypad::Socket> tcp;
    std::unique_ptr<lilypad::Socket> udp;
    sockaddr_in                      udp_dest{};

    // User list
    std::mutex              users_mutex;
    std::vector<UserEntry>  users;

    // Chat messages
    std::mutex              chat_mutex;
    std::vector<ChatMessage> chat_messages;

    // Per-user volume (client_id → volume 0.0-2.0, default 1.0)
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

    // Screen send queue — decouples capture/encode from TCP sending
    std::mutex                          screen_send_mutex;
    std::condition_variable             screen_send_cv;
    std::deque<ScreenSendItem>          screen_send_queue;
    std::unique_ptr<std::thread>        screen_send_thread;

    // Screen sharing — outgoing (this client is sharing)
    std::atomic<bool> screen_sharing{false};
    std::atomic<int>  screen_resolution{0}; // 0=720p, 1=1080p (maps to ScreenResolution)
    std::atomic<int>  screen_fps{0};        // 0=30fps, 1=60fps (maps to ScreenFps)

    // Screen sharing — incoming (watching another client)
    std::atomic<uint32_t> watching_user_id{0};      // 0 = not watching
    std::mutex            screen_frame_mutex;
    std::vector<uint8_t>  screen_jpeg_buf;           // raw JPEG from network (decoded on main thread)
    bool                  screen_jpeg_new    = false;

    // Decoded RGBA buffer (produced by decode thread, consumed by main thread)
    std::condition_variable screen_decode_cv;
    std::vector<uint8_t>    screen_rgba_buf;
    int                     screen_rgba_w = 0;
    int                     screen_rgba_h = 0;
    bool                    screen_rgba_new = false;
    std::unique_ptr<std::thread> screen_decode_thread;

    // D3D11 texture for viewer (created/updated on main thread)
    ID3D11Texture2D*          screen_texture = nullptr;
    ID3D11ShaderResourceView* screen_srv     = nullptr;
    int                       screen_tex_w   = 0;
    int                       screen_tex_h   = 0;

    void add_system_msg(const std::string& text) {
        std::lock_guard<std::mutex> lk(chat_mutex);
        chat_messages.push_back({0, "", text, true});
        if (chat_messages.size() > 500)
            chat_messages.erase(chat_messages.begin());
    }

    void add_chat_msg(uint32_t sender_id, const std::string& name, const std::string& text) {
        std::lock_guard<std::mutex> lk(chat_mutex);
        chat_messages.push_back({sender_id, name, text, false});
        if (chat_messages.size() > 500)
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
//  Background update check (GitHub)
// ════════════════════════════════════════════════════════════════
static void check_for_update_thread(AppState& app) {
    HINTERNET inet = InternetOpenA("LilyPad", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!inet) return;

    HINTERNET url = InternetOpenUrlA(inet, UPDATE_CHECK_URL, nullptr, 0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!url) {
        InternetCloseHandle(inet);
        return;
    }

    // Read response (version.txt is tiny — a few hundred bytes at most)
    std::string body;
    char buf[1024];
    DWORD bytes_read = 0;
    while (InternetReadFile(url, buf, sizeof(buf), &bytes_read) && bytes_read > 0) {
        body.append(buf, bytes_read);
        if (body.size() > 4096) break; // safety limit
    }

    InternetCloseHandle(url);
    InternetCloseHandle(inet);

    if (body.empty()) return;

    // Parse: line 1 = version, line 2 = download URL
    auto nl = body.find('\n');
    if (nl == std::string::npos) return;

    std::string version = body.substr(0, nl);
    // Trim carriage return if present
    if (!version.empty() && version.back() == '\r') version.pop_back();

    std::string dl_url = body.substr(nl + 1);
    // Trim trailing whitespace/newlines
    while (!dl_url.empty() && (dl_url.back() == '\r' || dl_url.back() == '\n' || dl_url.back() == ' '))
        dl_url.pop_back();

    if (version.empty() || dl_url.empty()) return;

    if (is_newer_version(APP_VERSION, version)) {
        std::lock_guard<std::mutex> lk(app.update_mutex);
        app.update_version = version;
        app.update_url     = dl_url;
        app.update_available = true;
    }
}

// ════════════════════════════════════════════════════════════════
//  Network threads
// ════════════════════════════════════════════════════════════════

// ── Screen decode thread: decodes JPEG→RGBA off the main thread ──
static void screen_decode_thread_func(AppState& app) {
    while (app.running && app.connected) {
        std::vector<uint8_t> jpeg_copy;
        {
            std::unique_lock<std::mutex> lk(app.screen_frame_mutex);
            app.screen_decode_cv.wait_for(lk, std::chrono::milliseconds(5),
                [&] { return app.screen_jpeg_new || !app.connected || !app.running; });
            if (!app.connected || !app.running) break;
            if (!app.screen_jpeg_new || app.screen_jpeg_buf.empty()) continue;
            jpeg_copy.swap(app.screen_jpeg_buf);
            app.screen_jpeg_new = false;
        }

        int fw = 0, fh = 0;
        auto rgba = lilypad::decode_jpeg_to_rgba(jpeg_copy.data(), jpeg_copy.size(), fw, fh);

        if (!rgba.empty()) {
            std::lock_guard<std::mutex> lk(app.screen_frame_mutex);
            app.screen_rgba_buf = std::move(rgba);
            app.screen_rgba_w = fw;
            app.screen_rgba_h = fh;
            app.screen_rgba_new = true;
        }
    }
}

// ── Screen send thread: drains queue with audio priority, drops stale video frames ──
static void screen_send_thread_func(AppState& app) {
    while (app.running && app.connected && app.screen_sharing) {
        std::deque<ScreenSendItem> batch;
        {
            std::unique_lock<std::mutex> lk(app.screen_send_mutex);
            app.screen_send_cv.wait_for(lk, std::chrono::milliseconds(5),
                [&] { return !app.screen_send_queue.empty() || !app.screen_sharing; });
            batch.swap(app.screen_send_queue);
        }

        if (batch.empty()) continue;

        // Send ALL audio items first (small packets, latency-sensitive)
        for (auto& item : batch) {
            if (item.is_audio) {
                app.send_tcp(item.data);
            }
        }

        // Send only the NEWEST video frame (drop older ones to prevent queue buildup)
        for (auto it = batch.rbegin(); it != batch.rend(); ++it) {
            if (!it->is_audio) {
                app.send_tcp(it->data);
                break;
            }
        }
    }
}

// ── Screen capture thread (runs while this client is sharing) ──
static void screen_capture_thread_func(AppState& app) {
    using clock = std::chrono::steady_clock;
    auto next_frame = clock::now();

    // Persistent capturer — uses DXGI Desktop Duplication (GPU-accelerated),
    // falls back to GDI if DXGI is unavailable.
    lilypad::ScreenCapturer capturer;

    // Adaptive JPEG quality — starts high, reduces when frames are large or backing up
    int quality = 85;
    static constexpr int    QUALITY_MIN      = 40;
    static constexpr int    QUALITY_MAX      = 92;
    static constexpr size_t FRAME_SIZE_HIGH  = 300000; // 300KB — reduce quality
    static constexpr size_t FRAME_SIZE_LOW   = 120000; // 120KB — increase quality

    while (app.running && app.connected && app.screen_sharing) {
        auto res = static_cast<lilypad::ScreenResolution>(app.screen_resolution.load());
        auto fps = static_cast<lilypad::ScreenFps>(app.screen_fps.load());

        int target_w = 0, target_h = 0;
        lilypad::get_capture_dimensions(res, target_w, target_h);
        int interval_ms = lilypad::get_capture_interval_ms(fps);

        next_frame += std::chrono::milliseconds(interval_ms);

        // Skip this frame if a video frame is still queued (sender can't keep up)
        {
            std::lock_guard<std::mutex> lk(app.screen_send_mutex);
            bool has_pending_frame = false;
            for (auto& item : app.screen_send_queue) {
                if (!item.is_audio) { has_pending_frame = true; break; }
            }
            if (has_pending_frame) {
                // Queue backed up — drop this frame and reduce quality
                quality = (std::max)(QUALITY_MIN, quality - 5);
                auto now = clock::now();
                if (next_frame > now)
                    std::this_thread::sleep_until(next_frame);
                else
                    next_frame = now;
                continue;
            }
        }

        int w = 0, h = 0;
        auto jpeg = capturer.capture_jpeg(quality, target_w, target_h, w, h);

        if (!jpeg.empty()) {
            // Adaptive quality: adjust based on encoded frame size
            if (jpeg.size() > FRAME_SIZE_HIGH) {
                quality = (std::max)(QUALITY_MIN, quality - 3);
            } else if (jpeg.size() < FRAME_SIZE_LOW) {
                quality = (std::min)(QUALITY_MAX, quality + 2);
            }

            auto msg = lilypad::make_screen_frame_msg(
                static_cast<uint16_t>(w), static_cast<uint16_t>(h),
                jpeg.data(), jpeg.size());

            {
                std::lock_guard<std::mutex> lk(app.screen_send_mutex);
                app.screen_send_queue.push_back({std::move(msg), false});
            }
            app.screen_send_cv.notify_one();
        }

        // Sleep until next frame time; if we're behind, reset to now
        auto now = clock::now();
        if (next_frame > now) {
            std::this_thread::sleep_until(next_frame);
        } else {
            next_frame = now;
        }
    }
}

// ── System audio capture thread (runs alongside screen sharing) ──
static void sys_audio_capture_thread_func(AppState& app) {
    try {
        SystemAudioCapture capture;
        lilypad::OpusEncoderWrapper encoder;

        // Accumulate mono samples until we have a full 960-sample (20ms) frame
        std::vector<float> accum;
        accum.reserve(lilypad::FRAME_SIZE * 2);

        while (app.running && app.connected && app.screen_sharing) {
            auto samples = capture.read_samples();
            if (!samples.empty()) {
                accum.insert(accum.end(), samples.begin(), samples.end());
            }

            // Encode and send complete frames
            while (accum.size() >= static_cast<size_t>(lilypad::FRAME_SIZE)) {
                auto opus_data = encoder.encode(accum.data(), lilypad::FRAME_SIZE);
                accum.erase(accum.begin(), accum.begin() + lilypad::FRAME_SIZE);

                auto msg = lilypad::make_screen_audio_msg(opus_data.data(), opus_data.size());
                {
                    std::lock_guard<std::mutex> lk(app.screen_send_mutex);
                    app.screen_send_queue.push_back({std::move(msg), true});
                }
                app.screen_send_cv.notify_one();
            }

            // Sleep briefly if no data was available
            if (samples.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
    } catch (...) {
        // System audio capture failed — silently stop
    }
}

static void tcp_receive_thread(AppState& app) {
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
            if (payload.size() > 4) {
                uint32_t uid = lilypad::read_u32(payload.data());
                std::string text(reinterpret_cast<const char*>(payload.data() + 4));
                std::string name = app.lookup_username(uid);
                if (uid == app.my_id) name = app.my_username;
                app.add_chat_msg(uid, name, text);
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
            // Server relay: sharer_id(4) + width(2) + height(2) + jpeg
            // Just store the raw JPEG — decode happens on the main thread
            if (payload.size() >= 8) {
                uint32_t sharer_id = lilypad::read_u32(payload.data());
                if (sharer_id == app.watching_user_id.load()) {
                    const uint8_t* jpeg = payload.data() + 8;
                    size_t jpeg_len = payload.size() - 8;

                    {
                        std::lock_guard<std::mutex> lk(app.screen_frame_mutex);
                        app.screen_jpeg_buf.assign(jpeg, jpeg + jpeg_len);
                        app.screen_jpeg_new = true;
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
        default:
            break;
        }
    }
}

static void voice_send_thread(AppState& app) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    lilypad::OpusEncoderWrapper encoder;
    uint32_t sequence = 0;

    // RNNoise state — created once, reused for the lifetime of the thread
    DenoiseState* rnn_st = rnnoise_create(nullptr);
    const int rnn_frame = 480; // rnnoise processes 480 samples at a time

    while (app.running && app.connected) {
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
static void udp_receive_thread_func(AppState& app) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    uint8_t buf[lilypad::MAX_VOICE_PACKET];

    while (app.running && app.connected) {
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
static void audio_playback_thread_func(AppState& app) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    const size_t frame_len = lilypad::FRAME_SIZE * lilypad::CHANNELS;

    while (app.running && app.connected) {
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
                    // Buffer underrun — use Opus PLC for interpolated audio
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
                auto& sa = app.sys_audio_frames.front();
                for (size_t i = 0; i < frame_len && i < sa.size(); ++i) {
                    mixed[i] += sa[i];
                }
                app.sys_audio_frames.pop_front();
            }
        }

        // Clamp
        for (float& s : mixed) {
            s = std::clamp(s, -1.0f, 1.0f);
        }

        try {
            // Blocking write — paces this thread at hardware rate (~20ms)
            app.playback->write_frame(mixed);
        } catch (...) { break; }
    }
}

// ════════════════════════════════════════════════════════════════
//  Connect / Disconnect
// ════════════════════════════════════════════════════════════════

static void do_connect(AppState& app, const std::string& server_ip, const std::string& username,
                       int input_device, int output_device) {
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

        auto capture  = std::make_unique<lilypad::AudioCapture>(
            lilypad::SAMPLE_RATE, lilypad::CHANNELS, lilypad::FRAME_SIZE, input_device);
        auto playback = std::make_unique<lilypad::AudioPlayback>(
            lilypad::SAMPLE_RATE, lilypad::CHANNELS, lilypad::FRAME_SIZE, output_device);

        app.tcp      = std::move(tcp);
        app.udp      = std::move(udp);
        app.udp_dest = udp_dest;
        app.my_id    = my_id;
        app.my_username = username;
        app.capture  = std::move(capture);
        app.playback = std::move(playback);

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
        {
            std::lock_guard<std::mutex> lk(app.screen_frame_mutex);
            app.screen_jpeg_buf.clear();
            app.screen_jpeg_new = false;
        }

        app.connected = true;
        app.add_system_msg("Connected! Your ID: " + std::to_string(my_id));

        app.tcp_thread     = std::make_unique<std::thread>(tcp_receive_thread, std::ref(app));
        app.send_thread    = std::make_unique<std::thread>(voice_send_thread, std::ref(app));
        app.udp_recv_thread    = std::make_unique<std::thread>(udp_receive_thread_func, std::ref(app));
        app.playback_thread    = std::make_unique<std::thread>(audio_playback_thread_func, std::ref(app));
        app.screen_decode_thread = std::make_unique<std::thread>(screen_decode_thread_func, std::ref(app));

    } catch (const std::exception& e) {
        app.add_system_msg(std::string("Connection error: ") + e.what());
    }
}

static void do_disconnect(AppState& app) {
    if (!app.connected) return;

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
    if (app.send_thread && app.send_thread->joinable()) app.send_thread->join();
    if (app.udp_recv_thread && app.udp_recv_thread->joinable()) app.udp_recv_thread->join();
    if (app.playback_thread && app.playback_thread->joinable()) app.playback_thread->join();
    if (app.screen_thread && app.screen_thread->joinable()) app.screen_thread->join();
    if (app.sys_audio_thread && app.sys_audio_thread->joinable()) app.sys_audio_thread->join();
    if (app.screen_send_thread && app.screen_send_thread->joinable()) app.screen_send_thread->join();
    if (app.screen_decode_thread && app.screen_decode_thread->joinable()) app.screen_decode_thread->join();

    app.tcp_thread.reset();
    app.send_thread.reset();
    app.udp_recv_thread.reset();
    app.playback_thread.reset();
    app.screen_thread.reset();
    app.sys_audio_thread.reset();
    app.screen_send_thread.reset();
    app.screen_decode_thread.reset();
    app.capture.reset();
    app.playback.reset();
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

    // Clean up screen viewer texture
    if (app.screen_srv) { app.screen_srv->Release(); app.screen_srv = nullptr; }
    if (app.screen_texture) { app.screen_texture->Release(); app.screen_texture = nullptr; }
    app.screen_tex_w = 0;
    app.screen_tex_h = 0;
    {
        std::lock_guard<std::mutex> lk(app.screen_frame_mutex);
        app.screen_jpeg_buf.clear();
        app.screen_jpeg_new = false;
        app.screen_rgba_buf.clear();
        app.screen_rgba_new = false;
        app.screen_rgba_w = 0;
        app.screen_rgba_h = 0;
    }

    {
        std::lock_guard<std::mutex> lk(app.users_mutex);
        app.users.clear();
    }

    app.add_system_msg("Disconnected.");
}

// ════════════════════════════════════════════════════════════════
//  PTT key names
// ════════════════════════════════════════════════════════════════
struct PttKeyOption {
    int         vk;
    const char* name;
};

static const PttKeyOption g_ptt_keys[] = {
    { 0x56, "V" },
    { 0x42, "B" },
    { 0x47, "G" },
    { 0x54, "T" },
    { VK_CAPITAL, "Caps Lock" },
    { VK_XBUTTON1, "Mouse 4" },
    { VK_XBUTTON2, "Mouse 5" },
};
static const int g_ptt_key_count = sizeof(g_ptt_keys) / sizeof(g_ptt_keys[0]);

// ════════════════════════════════════════════════════════════════
//  Theme
// ════════════════════════════════════════════════════════════════
static void ApplyLilyPadTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors    = style.Colors;

    style.WindowRounding    = 6.0f;
    style.FrameRounding     = 4.0f;
    style.GrabRounding      = 3.0f;
    style.PopupRounding     = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.TabRounding       = 4.0f;
    style.WindowPadding     = ImVec2(12, 12);
    style.FramePadding      = ImVec2(8, 5);
    style.ItemSpacing       = ImVec2(8, 6);

    ImVec4 bg_dark   = ImVec4(0.08f, 0.08f, 0.10f, 1.00f);
    ImVec4 bg_mid    = ImVec4(0.12f, 0.12f, 0.14f, 1.00f);
    ImVec4 bg_light  = ImVec4(0.18f, 0.18f, 0.21f, 1.00f);
    ImVec4 accent    = ImVec4(0.33f, 0.72f, 0.48f, 1.00f);
    ImVec4 accent_hi = ImVec4(0.40f, 0.82f, 0.55f, 1.00f);
    ImVec4 accent_lo = ImVec4(0.25f, 0.55f, 0.38f, 1.00f);
    ImVec4 text      = ImVec4(0.90f, 0.92f, 0.90f, 1.00f);
    ImVec4 text_dim  = ImVec4(0.55f, 0.57f, 0.55f, 1.00f);
    ImVec4 border    = ImVec4(0.22f, 0.22f, 0.25f, 1.00f);

    colors[ImGuiCol_WindowBg]             = bg_dark;
    colors[ImGuiCol_ChildBg]              = bg_mid;
    colors[ImGuiCol_PopupBg]              = ImVec4(0.10f, 0.10f, 0.12f, 0.96f);
    colors[ImGuiCol_Border]               = border;
    colors[ImGuiCol_FrameBg]              = bg_light;
    colors[ImGuiCol_FrameBgHovered]       = ImVec4(0.24f, 0.24f, 0.28f, 1.00f);
    colors[ImGuiCol_FrameBgActive]        = ImVec4(0.28f, 0.28f, 0.33f, 1.00f);
    colors[ImGuiCol_TitleBg]              = bg_dark;
    colors[ImGuiCol_TitleBgActive]        = bg_mid;
    colors[ImGuiCol_TitleBgCollapsed]     = bg_dark;
    colors[ImGuiCol_MenuBarBg]            = bg_mid;
    colors[ImGuiCol_ScrollbarBg]          = bg_dark;
    colors[ImGuiCol_ScrollbarGrab]        = bg_light;
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.28f, 0.28f, 0.32f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]  = accent_lo;
    colors[ImGuiCol_CheckMark]            = accent;
    colors[ImGuiCol_SliderGrab]           = accent;
    colors[ImGuiCol_SliderGrabActive]     = accent_hi;
    colors[ImGuiCol_Button]               = bg_light;
    colors[ImGuiCol_ButtonHovered]        = ImVec4(0.25f, 0.25f, 0.30f, 1.00f);
    colors[ImGuiCol_ButtonActive]         = accent_lo;
    colors[ImGuiCol_Header]               = bg_light;
    colors[ImGuiCol_HeaderHovered]        = ImVec4(0.24f, 0.24f, 0.28f, 1.00f);
    colors[ImGuiCol_HeaderActive]         = accent_lo;
    colors[ImGuiCol_Separator]            = border;
    colors[ImGuiCol_SeparatorHovered]     = accent;
    colors[ImGuiCol_SeparatorActive]      = accent_hi;
    colors[ImGuiCol_ResizeGrip]           = ImVec4(0.20f, 0.20f, 0.23f, 0.50f);
    colors[ImGuiCol_ResizeGripHovered]    = accent;
    colors[ImGuiCol_ResizeGripActive]     = accent_hi;
    colors[ImGuiCol_Tab]                  = bg_light;
    colors[ImGuiCol_TabHovered]           = accent;
    colors[ImGuiCol_TabSelected]          = accent_lo;
    colors[ImGuiCol_Text]                 = text;
    colors[ImGuiCol_TextDisabled]         = text_dim;
}

// ════════════════════════════════════════════════════════════════
//  WinMain
// ════════════════════════════════════════════════════════════════

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    lilypad::WinsockInit winsock;
    lilypad::PortAudioInit pa;

    WNDCLASSEX wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_CLASSDC;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance      = hInstance;
    wc.lpszClassName  = _T("LilyPadClient");
    wc.hCursor        = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassEx(&wc);

    HWND hwnd = CreateWindow(wc.lpszClassName, _T("LilyPad Voice Chat"),
        WS_OVERLAPPEDWINDOW, 100, 100, 900, 620,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateD3DDevice(hwnd)) {
        CleanupD3D();
        UnregisterClass(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ApplyLilyPadTheme();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_d3d_device, g_d3d_context);

    // ── State ──
    AppState app;

    // Check for updates from GitHub in a background thread
    std::thread(check_for_update_thread, std::ref(app)).detach();

    char ip_buf[64]       = "127.0.0.1";
    char username_buf[64] = "";
    char chat_input[512]  = "";

    auto input_devices  = lilypad::get_input_devices();
    auto output_devices = lilypad::get_output_devices();

    int selected_input  = -1;
    int selected_output = -1;

    int default_in  = lilypad::get_default_input_device();
    int default_out = lilypad::get_default_output_device();
    for (int i = 0; i < static_cast<int>(input_devices.size()); ++i) {
        if (input_devices[i].index == default_in) { selected_input = i; break; }
    }
    for (int i = 0; i < static_cast<int>(output_devices.size()); ++i) {
        if (output_devices[i].index == default_out) { selected_output = i; break; }
    }

    // PTT UI state
    bool ptt_enabled = false;
    int  ptt_key_sel = 0; // index into g_ptt_keys
    bool noise_suppression = false;

    // Screen sharing UI state
    int screen_res_sel = 0; // 0=720p, 1=1080p
    int screen_fps_sel = 0; // 0=30fps, 1=60fps
    static const char* screen_res_names[] = { "720p", "1080p" };
    static const char* screen_fps_names[] = { "30 FPS", "60 FPS" };

    bool scroll_chat_to_bottom = true;

    // Server favorites
    auto favorites = load_favorites();
    int  selected_fav = -1;
    char fav_name_buf[64] = "";

    // ── Main loop ──
    ImVec4 clear_color = ImVec4(0.06f, 0.06f, 0.08f, 1.00f);
    bool running = true;

    while (running) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                running = false;
        }
        if (!running) break;

        // Poll PTT key state
        if (app.ptt_enabled) {
            bool held = (GetAsyncKeyState(app.ptt_key.load()) & 0x8000) != 0;
            app.ptt_active = held;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // ── Upload decoded RGBA to D3D11 texture (decode happens on separate thread) ──
        {
            std::vector<uint8_t> rgba;
            int fw = 0, fh = 0;
            {
                std::lock_guard<std::mutex> lk(app.screen_frame_mutex);
                if (app.screen_rgba_new && !app.screen_rgba_buf.empty()) {
                    rgba.swap(app.screen_rgba_buf);
                    fw = app.screen_rgba_w;
                    fh = app.screen_rgba_h;
                    app.screen_rgba_new = false;
                }
            }

            if (!rgba.empty()) {
                // Recreate texture if size changed
                if (fw != app.screen_tex_w || fh != app.screen_tex_h) {
                    if (app.screen_srv) { app.screen_srv->Release(); app.screen_srv = nullptr; }
                    if (app.screen_texture) { app.screen_texture->Release(); app.screen_texture = nullptr; }

                    D3D11_TEXTURE2D_DESC desc{};
                    desc.Width            = fw;
                    desc.Height           = fh;
                    desc.MipLevels        = 1;
                    desc.ArraySize        = 1;
                    desc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
                    desc.SampleDesc.Count = 1;
                    desc.Usage            = D3D11_USAGE_DYNAMIC;
                    desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
                    desc.CPUAccessFlags   = D3D11_CPU_ACCESS_WRITE;

                    g_d3d_device->CreateTexture2D(&desc, nullptr, &app.screen_texture);
                    if (app.screen_texture) {
                        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
                        srv_desc.Format                    = desc.Format;
                        srv_desc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
                        srv_desc.Texture2D.MipLevels       = 1;
                        g_d3d_device->CreateShaderResourceView(app.screen_texture, &srv_desc, &app.screen_srv);
                    }

                    app.screen_tex_w = fw;
                    app.screen_tex_h = fh;
                }

                // Upload pixel data
                if (app.screen_texture) {
                    D3D11_MAPPED_SUBRESOURCE mapped{};
                    HRESULT hr = g_d3d_context->Map(app.screen_texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
                    if (SUCCEEDED(hr)) {
                        for (int y = 0; y < fh; ++y) {
                            memcpy(static_cast<uint8_t*>(mapped.pData) + y * mapped.RowPitch,
                                   rgba.data() + y * fw * 4,
                                   fw * 4);
                        }
                        g_d3d_context->Unmap(app.screen_texture, 0);
                    }
                }
            }
        }

        // ── Full-window panel ──
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("##Main", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoBringToFrontOnFocus);

        // Title bar
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.33f, 0.72f, 0.48f, 1.00f));
            ImGui::Text("LilyPad");
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextDisabled("Voice Chat");

            // Status indicator on the right
            if (app.connected) {
                ImGui::SameLine(ImGui::GetWindowWidth() - 140);
                if (app.muted) {
                    ImGui::TextColored(ImVec4(0.72f, 0.28f, 0.28f, 1.0f), "[MUTED]");
                } else if (app.ptt_enabled) {
                    if (app.ptt_active) {
                        ImGui::TextColored(ImVec4(0.40f, 0.82f, 0.55f, 1.0f), "[TRANSMITTING]");
                    } else {
                        ImGui::TextDisabled("[PTT: %s]", g_ptt_keys[ptt_key_sel].name);
                    }
                }
            }
        }
        ImGui::Separator();
        ImGui::Spacing();

        bool is_connected = app.connected.load();

        // ════════════════════════════════════════
        //  Left panel: Connection / Users / Settings
        // ════════════════════════════════════════
        float panel_width = 260.0f;
        ImGui::BeginChild("##LeftPanel", ImVec2(panel_width, 0), true);

        ImGui::TextColored(ImVec4(0.33f, 0.72f, 0.48f, 1.0f), "Connection");
        ImGui::Separator();
        ImGui::Spacing();

        if (!is_connected) {
            ImGui::Text("Server IP");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##ip", ip_buf, sizeof(ip_buf));

            // Favorites
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.33f, 0.72f, 0.48f, 1.0f), "Favorites");
            ImGui::Separator();
            ImGui::Spacing();

            if (!favorites.empty()) {
                const char* fav_preview = (selected_fav >= 0 && selected_fav < static_cast<int>(favorites.size()))
                    ? favorites[selected_fav].name.c_str() : "Select a server...";
                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo("##favs", fav_preview)) {
                    for (int i = 0; i < static_cast<int>(favorites.size()); ++i) {
                        bool sel = (selected_fav == i);
                        std::string label = favorites[i].name + "  (" + favorites[i].ip + ")";
                        if (ImGui::Selectable(label.c_str(), sel)) {
                            selected_fav = i;
                            // Copy selected IP and username into the fields
                            strncpy(ip_buf, favorites[i].ip.c_str(), sizeof(ip_buf) - 1);
                            ip_buf[sizeof(ip_buf) - 1] = '\0';
                            if (!favorites[i].username.empty()) {
                                strncpy(username_buf, favorites[i].username.c_str(), sizeof(username_buf) - 1);
                                username_buf[sizeof(username_buf) - 1] = '\0';
                            }
                        }
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }

                // Remove button
                if (selected_fav >= 0 && selected_fav < static_cast<int>(favorites.size())) {
                    if (ImGui::SmallButton("Remove")) {
                        favorites.erase(favorites.begin() + selected_fav);
                        selected_fav = -1;
                        save_favorites(favorites);
                    }
                }

                ImGui::Spacing();
            }

            // Save current IP to favorites
            ImGui::Text("Name");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##fav_name", fav_name_buf, sizeof(fav_name_buf));
            if (ImGui::SmallButton("Save to Favorites")) {
                std::string ip_str(ip_buf);
                if (!ip_str.empty()) {
                    std::string name_str(fav_name_buf);
                    if (name_str.empty()) name_str = ip_str;
                    std::string user_str(username_buf);
                    favorites.push_back({name_str, ip_str, user_str});
                    save_favorites(favorites);
                    fav_name_buf[0] = '\0';
                    selected_fav = static_cast<int>(favorites.size()) - 1;
                }
            }

            ImGui::Spacing();
            ImGui::Spacing();

            ImGui::Text("Username");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##user", username_buf, sizeof(username_buf));

            ImGui::Spacing();
            ImGui::Spacing();

            // Audio devices
            ImGui::TextColored(ImVec4(0.33f, 0.72f, 0.48f, 1.0f), "Audio Devices");
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::Text("Input");
            ImGui::SetNextItemWidth(-1);
            const char* in_preview = (selected_input >= 0 && selected_input < static_cast<int>(input_devices.size()))
                ? input_devices[selected_input].name.c_str() : "Default";
            if (ImGui::BeginCombo("##in_dev", in_preview)) {
                for (int i = 0; i < static_cast<int>(input_devices.size()); ++i) {
                    bool sel = (selected_input == i);
                    if (ImGui::Selectable(input_devices[i].name.c_str(), sel))
                        selected_input = i;
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            ImGui::Spacing();
            ImGui::Text("Output");
            ImGui::SetNextItemWidth(-1);
            const char* out_preview = (selected_output >= 0 && selected_output < static_cast<int>(output_devices.size()))
                ? output_devices[selected_output].name.c_str() : "Default";
            if (ImGui::BeginCombo("##out_dev", out_preview)) {
                for (int i = 0; i < static_cast<int>(output_devices.size()); ++i) {
                    bool sel = (selected_output == i);
                    if (ImGui::Selectable(output_devices[i].name.c_str(), sel))
                        selected_output = i;
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            ImGui::Spacing();
            ImGui::Spacing();

            // PTT settings (before connect)
            ImGui::TextColored(ImVec4(0.33f, 0.72f, 0.48f, 1.0f), "Voice Mode");
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::Checkbox("Push-to-Talk", &ptt_enabled);
            if (ptt_enabled) {
                ImGui::Text("PTT Key");
                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo("##ptt_key", g_ptt_keys[ptt_key_sel].name)) {
                    for (int i = 0; i < g_ptt_key_count; ++i) {
                        bool sel = (ptt_key_sel == i);
                        if (ImGui::Selectable(g_ptt_keys[i].name, sel))
                            ptt_key_sel = i;
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }

            ImGui::Spacing();
            ImGui::Checkbox("Noise Suppression", &noise_suppression);

            ImGui::Spacing();
            ImGui::Spacing();

            // Update available button (pre-connection)
            if (app.update_available.load()) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70f, 0.55f, 0.15f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.68f, 0.20f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.95f, 0.78f, 0.25f, 1.0f));
                char update_label[128];
                {
                    std::lock_guard<std::mutex> lk(app.update_mutex);
                    snprintf(update_label, sizeof(update_label), "Update Available (%s)", app.update_version.c_str());
                }
                if (ImGui::Button(update_label, ImVec2(-1, 30))) {
                    std::lock_guard<std::mutex> lk(app.update_mutex);
                    ShellExecuteA(nullptr, "open", app.update_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
                ImGui::PopStyleColor(3);
                ImGui::Spacing();
            }

            // Connect button
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.55f, 0.38f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.33f, 0.72f, 0.48f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.40f, 0.82f, 0.55f, 1.0f));
            if (ImGui::Button("Connect", ImVec2(-1, 36))) {
                app.ptt_enabled = ptt_enabled;
                app.ptt_key     = g_ptt_keys[ptt_key_sel].vk;
                app.noise_suppression = noise_suppression;
                int in_dev  = (selected_input >= 0 && selected_input < static_cast<int>(input_devices.size()))
                    ? input_devices[selected_input].index : -1;
                int out_dev = (selected_output >= 0 && selected_output < static_cast<int>(output_devices.size()))
                    ? output_devices[selected_output].index : -1;
                do_connect(app, ip_buf, username_buf, in_dev, out_dev);
            }
            ImGui::PopStyleColor(3);

        } else {
            // ── Connected state ──
            ImGui::TextColored(ImVec4(0.33f, 0.72f, 0.48f, 1.0f), "Connected");
            ImGui::Text("Server: %s", ip_buf);
            ImGui::Text("Your ID: %u", app.my_id);

            // Update available button
            if (app.update_available.load()) {
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70f, 0.55f, 0.15f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.68f, 0.20f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.95f, 0.78f, 0.25f, 1.0f));
                char update_label[128];
                {
                    std::lock_guard<std::mutex> lk(app.update_mutex);
                    snprintf(update_label, sizeof(update_label), "Update Available (%s)", app.update_version.c_str());
                }
                if (ImGui::Button(update_label, ImVec2(-1, 30))) {
                    std::lock_guard<std::mutex> lk(app.update_mutex);
                    ShellExecuteA(nullptr, "open", app.update_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
                ImGui::PopStyleColor(3);
            }

            ImGui::Spacing();

            // Mute button
            {
                bool is_muted = app.muted.load();
                if (is_muted) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.22f, 0.22f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.72f, 0.28f, 0.28f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.82f, 0.33f, 0.33f, 1.0f));
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.55f, 0.38f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.33f, 0.72f, 0.48f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.40f, 0.82f, 0.55f, 1.0f));
                }
                if (ImGui::Button(is_muted ? "Unmute" : "Mute", ImVec2(-1, 30))) {
                    app.muted = !is_muted;
                }
                ImGui::PopStyleColor(3);
            }

            ImGui::Spacing();
            ImGui::Spacing();

            // Voice mode toggle (live)
            ImGui::TextColored(ImVec4(0.33f, 0.72f, 0.48f, 1.0f), "Voice Mode");
            ImGui::Separator();
            ImGui::Spacing();

            if (ImGui::Checkbox("Push-to-Talk##live", &ptt_enabled)) {
                app.ptt_enabled = ptt_enabled;
            }
            if (ptt_enabled) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(90);
                if (ImGui::BeginCombo("##ptt_key_live", g_ptt_keys[ptt_key_sel].name)) {
                    for (int i = 0; i < g_ptt_key_count; ++i) {
                        bool sel = (ptt_key_sel == i);
                        if (ImGui::Selectable(g_ptt_keys[i].name, sel)) {
                            ptt_key_sel = i;
                            app.ptt_key = g_ptt_keys[i].vk;
                        }
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }

            ImGui::Spacing();
            if (ImGui::Checkbox("Noise Suppression##live", &noise_suppression)) {
                app.noise_suppression = noise_suppression;
            }

            ImGui::Spacing();
            ImGui::Spacing();

            // Screen sharing
            ImGui::TextColored(ImVec4(0.33f, 0.72f, 0.48f, 1.0f), "Screen Sharing");
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::Text("Resolution");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##screen_res", screen_res_names[screen_res_sel])) {
                for (int i = 0; i < 2; ++i) {
                    bool sel = (screen_res_sel == i);
                    if (ImGui::Selectable(screen_res_names[i], sel)) {
                        screen_res_sel = i;
                        app.screen_resolution = i;
                    }
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            ImGui::Spacing();
            ImGui::Text("Frame Rate");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##screen_fps", screen_fps_names[screen_fps_sel])) {
                for (int i = 0; i < 2; ++i) {
                    bool sel = (screen_fps_sel == i);
                    if (ImGui::Selectable(screen_fps_names[i], sel)) {
                        screen_fps_sel = i;
                        app.screen_fps = i;
                    }
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            ImGui::Spacing();

            if (app.screen_sharing.load()) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.22f, 0.22f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.72f, 0.28f, 0.28f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.82f, 0.33f, 0.33f, 1.0f));
                if (ImGui::Button("Stop Sharing", ImVec2(-1, 30))) {
                    app.screen_sharing = false;
                    app.screen_send_cv.notify_all(); // wake send thread to exit
                    // Wait for threads to finish
                    if (app.screen_thread && app.screen_thread->joinable())
                        app.screen_thread->join();
                    app.screen_thread.reset();
                    if (app.sys_audio_thread && app.sys_audio_thread->joinable())
                        app.sys_audio_thread->join();
                    app.sys_audio_thread.reset();
                    if (app.screen_send_thread && app.screen_send_thread->joinable())
                        app.screen_send_thread->join();
                    app.screen_send_thread.reset();
                    // Clear any leftover items in the queue
                    {
                        std::lock_guard<std::mutex> lk(app.screen_send_mutex);
                        app.screen_send_queue.clear();
                    }
                    app.send_tcp(lilypad::make_screen_stop_msg());
                }
                ImGui::PopStyleColor(3);
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.40f, 0.55f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.52f, 0.70f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.33f, 0.60f, 0.80f, 1.0f));
                if (ImGui::Button("Share Screen", ImVec2(-1, 30))) {
                    app.screen_sharing = true;
                    app.send_tcp(lilypad::make_screen_start_msg());
                    app.screen_send_thread = std::make_unique<std::thread>(screen_send_thread_func, std::ref(app));
                    app.screen_thread = std::make_unique<std::thread>(screen_capture_thread_func, std::ref(app));
                    app.sys_audio_thread = std::make_unique<std::thread>(sys_audio_capture_thread_func, std::ref(app));
                }
                ImGui::PopStyleColor(3);
            }

            ImGui::Spacing();
            ImGui::Spacing();

            // User list with right-click volume popup
            ImGui::TextColored(ImVec4(0.33f, 0.72f, 0.48f, 1.0f), "Users Online");
            ImGui::Separator();
            ImGui::Spacing();

            {
                std::lock_guard<std::mutex> lk(app.users_mutex);
                if (app.users.empty()) {
                    ImGui::TextDisabled("No other users.");
                } else {
                    for (auto& u : app.users) {
                        ImGui::PushID(static_cast<int>(u.id));

                        // User row — talking indicator
                        bool is_talking = false;
                        {
                            std::lock_guard<std::mutex> va_lk(app.voice_activity_mutex);
                            auto va_it = app.voice_last_seen.find(u.id);
                            if (va_it != app.voice_last_seen.end()) {
                                auto elapsed = std::chrono::steady_clock::now() - va_it->second;
                                is_talking = elapsed < std::chrono::milliseconds(300);
                            }
                        }
                        float vol = app.get_volume(u.id);
                        ImVec4 dot_color;
                        if (vol < 0.01f) {
                            dot_color = ImVec4(0.55f, 0.22f, 0.22f, 1.0f); // muted (red)
                        } else if (is_talking) {
                            dot_color = ImVec4(0.40f, 0.82f, 0.55f, 1.0f); // talking (bright green)
                        } else {
                            dot_color = ImVec4(0.35f, 0.35f, 0.38f, 1.0f); // idle (gray)
                        }
                        ImGui::TextColored(dot_color, "  *");
                        ImGui::SameLine();
                        ImGui::Text("%s", u.name.c_str());
                        ImGui::SameLine();
                        ImGui::TextDisabled("(#%u)", u.id);

                        // Screen sharing Watch/Stop button
                        if (u.is_sharing) {
                            ImGui::SameLine();
                            uint32_t watching = app.watching_user_id.load();
                            if (watching == u.id) {
                                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.22f, 0.22f, 1.0f));
                                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.72f, 0.28f, 0.28f, 1.0f));
                                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.82f, 0.33f, 0.33f, 1.0f));
                                if (ImGui::SmallButton("Stop")) {
                                    app.send_tcp(lilypad::make_screen_unsubscribe_msg(u.id));
                                    app.watching_user_id = 0;
                                }
                                ImGui::PopStyleColor(3);
                            } else {
                                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.40f, 0.55f, 1.0f));
                                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.52f, 0.70f, 1.0f));
                                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.33f, 0.60f, 0.80f, 1.0f));
                                if (ImGui::SmallButton("Watch")) {
                                    // Unsubscribe from previous if any
                                    if (watching != 0) {
                                        app.send_tcp(lilypad::make_screen_unsubscribe_msg(watching));
                                    }
                                    app.watching_user_id = u.id;
                                    app.send_tcp(lilypad::make_screen_subscribe_msg(u.id));
                                }
                                ImGui::PopStyleColor(3);
                            }
                        }

                        // Right-click context menu
                        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                            ImGui::OpenPopup("##vol_popup");
                        }

                        if (ImGui::BeginPopup("##vol_popup")) {
                            ImGui::Text("Volume: %s", u.name.c_str());
                            ImGui::Separator();
                            float vol_pct = vol * 100.0f;
                            ImGui::SetNextItemWidth(180);
                            if (ImGui::SliderFloat("##vol", &vol_pct, 0.0f, 200.0f, "%.0f%%",
                                    ImGuiSliderFlags_AlwaysClamp)) {
                                app.set_volume(u.id, vol_pct / 100.0f);
                            }
                            if (ImGui::Button("Reset to 100%", ImVec2(-1, 0))) {
                                app.set_volume(u.id, 1.0f);
                            }
                            ImGui::EndPopup();
                        }

                        ImGui::PopID();
                    }
                }
            }

            ImGui::Spacing();
            ImGui::Spacing();

            // Disconnect
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.22f, 0.22f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.72f, 0.28f, 0.28f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.82f, 0.33f, 0.33f, 1.0f));
            if (ImGui::Button("Disconnect", ImVec2(-1, 36))) {
                do_disconnect(app);
            }
            ImGui::PopStyleColor(3);
        }

        ImGui::EndChild();

        ImGui::SameLine();

        // ════════════════════════════════════════
        //  Right panel: Chat
        // ════════════════════════════════════════
        ImGui::BeginChild("##RightPanel", ImVec2(0, 0), true);

        ImGui::TextColored(ImVec4(0.33f, 0.72f, 0.48f, 1.0f), "Chat");
        ImGui::Separator();
        ImGui::Spacing();

        // Chat messages area (fill available height minus input bar)
        float input_height = 40.0f;
        ImGui::BeginChild("##ChatScroll", ImVec2(0, -input_height), false);
        {
            std::lock_guard<std::mutex> lk(app.chat_mutex);
            for (auto& m : app.chat_messages) {
                if (m.is_system) {
                    ImGui::TextDisabled("  %s", m.text.c_str());
                } else {
                    // Sender name in accent color
                    bool is_me = (m.sender_id == app.my_id);
                    ImVec4 name_color = is_me
                        ? ImVec4(0.55f, 0.75f, 0.95f, 1.0f)
                        : ImVec4(0.33f, 0.72f, 0.48f, 1.0f);
                    ImGui::TextColored(name_color, "%s:", m.sender_name.c_str());
                    ImGui::SameLine();
                    ImGui::TextWrapped("%s", m.text.c_str());
                }
            }
        }
        if (scroll_chat_to_bottom && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20.0f)
            ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();

        // Chat input bar
        ImGui::Separator();
        bool send_chat = false;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 70);
        if (ImGui::InputText("##chat_input", chat_input, sizeof(chat_input),
                ImGuiInputTextFlags_EnterReturnsTrue)) {
            send_chat = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Send", ImVec2(-1, 0))) {
            send_chat = true;
        }

        if (send_chat && chat_input[0] != '\0' && is_connected) {
            auto msg = lilypad::make_text_chat_msg(chat_input);
            app.send_tcp(msg);
            chat_input[0] = '\0';
            scroll_chat_to_bottom = true;
            // Re-focus the input field
            ImGui::SetKeyboardFocusHere(-1);
        }

        ImGui::EndChild();

        ImGui::End(); // Main window

        // ── Screen viewer floating window ──
        uint32_t watching_id = app.watching_user_id.load();
        if (watching_id != 0 && app.screen_srv && app.screen_tex_w > 0 && app.screen_tex_h > 0) {
            std::string watcher_name = app.lookup_username(watching_id);
            std::string win_title = "Screen: " + watcher_name;

            ImGui::SetNextWindowSize(ImVec2(640, 400), ImGuiCond_FirstUseEver);
            bool viewer_open = true;
            if (ImGui::Begin(win_title.c_str(), &viewer_open, ImGuiWindowFlags_NoCollapse)) {
                ImVec2 avail = ImGui::GetContentRegionAvail();
                float aspect = static_cast<float>(app.screen_tex_w) / static_cast<float>(app.screen_tex_h);
                float disp_w = avail.x;
                float disp_h = avail.x / aspect;
                if (disp_h > avail.y) {
                    disp_h = avail.y;
                    disp_w = avail.y * aspect;
                }
                // Center the image
                float offset_x = (avail.x - disp_w) * 0.5f;
                if (offset_x > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset_x);
                ImGui::Image((ImTextureID)(uintptr_t)app.screen_srv, ImVec2(disp_w, disp_h));
            }
            ImGui::End();

            if (!viewer_open) {
                app.send_tcp(lilypad::make_screen_unsubscribe_msg(watching_id));
                app.watching_user_id = 0;
            }
        }

        // ── Render ──
        ImGui::Render();
        g_d3d_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        float clear[4] = { clear_color.x, clear_color.y, clear_color.z, clear_color.w };
        g_d3d_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap_chain->Present(1, 0);
    }

    // ── Cleanup ──
    do_disconnect(app);
    app.running = false;

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupD3D();
    DestroyWindow(hwnd);
    UnregisterClass(wc.lpszClassName, wc.hInstance);

    return 0;
}
