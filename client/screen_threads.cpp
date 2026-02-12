#include "screen_threads.h"
#include "d3d_helpers.h"
#include "h264_encoder.h"
#include "h264_decoder.h"
#include "screen_capture.h"
#include "system_audio.h"

#include <mfapi.h>

#include <chrono>
#include <thread>

// ── Screen decode thread: decodes H.264->RGBA via Media Foundation ──
void screen_decode_thread_func(AppState& app) {
    (void)CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    lilypad::H264Decoder decoder;
    if (!decoder.init(g_d3d_device)) {
        app.add_system_msg("H.264 decoder init failed");
        CoUninitialize();
        return;
    }

    int frames_received = 0;
    int frames_decoded = 0;

    while (app.running && app.connected) {
        std::vector<uint8_t> frame_copy;
        uint8_t flags = 0;
        {
            std::unique_lock<std::mutex> lk(app.screen_frame_mutex);
            app.screen_decode_cv.wait_for(lk, std::chrono::milliseconds(5),
                [&] { return app.screen_frame_new || !app.connected || !app.running; });
            if (!app.connected || !app.running) break;
            if (!app.screen_frame_new || app.screen_frame_buf.empty()) continue;
            frame_copy.swap(app.screen_frame_buf);
            flags = app.screen_frame_flags;
            app.screen_frame_new = false;
        }

        frames_received++;
        bool is_keyframe = (flags & lilypad::SCREEN_FLAG_KEYFRAME) != 0;

        if (frames_received <= 3) {
            char msg[128];
            snprintf(msg, sizeof(msg), "[Viewer] Frame #%d: %zu bytes, flags=0x%02X%s",
                     frames_received, frame_copy.size(), flags, is_keyframe ? " (IDR)" : "");
            app.add_system_msg(msg);
        }

        if (decoder.decode(frame_copy.data(), frame_copy.size(), is_keyframe)) {
            frames_decoded++;
            std::lock_guard<std::mutex> lk(app.screen_srv_mutex);
            app.screen_srv   = decoder.get_output_srv();
            app.screen_srv_w = decoder.width();
            app.screen_srv_h = decoder.height();

            if (frames_decoded <= 3) {
                char msg[128];
                snprintf(msg, sizeof(msg), "[Viewer] Decoded #%d: %dx%d, SRV=%s",
                         frames_decoded, decoder.width(), decoder.height(),
                         decoder.get_output_srv() ? "OK" : "null");
                app.add_system_msg(msg);
            }
        } else if (frames_received <= 5) {
            app.add_system_msg("[Viewer] Decode failed for frame #" + std::to_string(frames_received));
        }
    }

    decoder.flush();
    decoder.shutdown();
    CoUninitialize();
}

// ── Screen send thread: drains queue with audio priority, drops stale video frames ──
void screen_send_thread_func(AppState& app) {
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
void screen_capture_thread_func(AppState& app) {
    (void)CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    using clock = std::chrono::steady_clock;
    auto next_frame = clock::now();

    lilypad::ScreenCapturer capturer;

    if (!capturer.get_device()) {
        app.add_system_msg("Screen capture init failed (no D3D11 device)");
        CoUninitialize();
        return;
    }

    // Encode at native capture resolution, 30fps (bitrate controls quality)
    constexpr int fps_val = 30;
    constexpr int interval_ms = 33;
    int enc_w = capturer.screen_width() & ~1;
    int enc_h = capturer.screen_height() & ~1;

    // Auto bitrate based on resolution
    int bitrate = app.h264_bitrate.load();
    if (bitrate <= 0) {
        int pixels = enc_w * enc_h;
        if (pixels >= 3686400)       bitrate = 30000000;  // 2560x1440+: 30 Mbps
        else if (pixels >= 2073600)  bitrate = 18000000;  // 1920x1080:  18 Mbps
        else if (pixels >= 921600)   bitrate = 10000000;  // 1280x720:   10 Mbps
        else                         bitrate = 6000000;   // smaller:    6 Mbps
        app.h264_bitrate.store(bitrate);
    }

    lilypad::H264Encoder encoder;
    if (!encoder.init(capturer.get_device(), enc_w, enc_h, fps_val, bitrate)) {
        app.add_system_msg("H.264 encoder init failed");
        CoUninitialize();
        return;
    }

    int cap_frame = 0;
    while (app.running && app.connected && app.screen_sharing) {
        next_frame += std::chrono::milliseconds(interval_ms);

        // Skip this frame if a video frame is still queued (sender can't keep up)
        {
            std::lock_guard<std::mutex> lk(app.screen_send_mutex);
            bool has_pending_frame = false;
            for (auto& item : app.screen_send_queue) {
                if (!item.is_audio) { has_pending_frame = true; break; }
            }
            if (has_pending_frame) {
                auto now = clock::now();
                if (next_frame > now)
                    std::this_thread::sleep_until(next_frame);
                else
                    next_frame = now;
                continue;
            }
        }

        // Update bitrate if changed
        int new_bitrate = app.h264_bitrate.load();
        if (new_bitrate != bitrate) {
            bitrate = new_bitrate;
            encoder.set_bitrate(bitrate);
        }

        int w = 0, h = 0;
        auto* tex = capturer.capture_texture(w, h);

        cap_frame++;

        if (tex) {
            bool force_idr = app.force_keyframe.exchange(false);
            bool is_keyframe = false;
            auto h264 = encoder.encode(tex, force_idr, is_keyframe);

            if (!h264.empty()) {
                uint8_t flags = is_keyframe ? lilypad::SCREEN_FLAG_KEYFRAME : 0;
                auto msg = lilypad::make_screen_frame_msg(
                    static_cast<uint16_t>(enc_w), static_cast<uint16_t>(enc_h),
                    flags, h264.data(), h264.size());

                {
                    std::lock_guard<std::mutex> lk(app.screen_send_mutex);
                    app.screen_send_queue.push_back({std::move(msg), false});
                }
                app.screen_send_cv.notify_one();
            } else if (cap_frame <= 10) {
                char dbg[128];
                snprintf(dbg, sizeof(dbg), "[CapThread] Frame %d: encode returned empty\n", cap_frame);
                OutputDebugStringA(dbg);
            }
        } else if (cap_frame <= 10) {
            char dbg[128];
            snprintf(dbg, sizeof(dbg), "[CapThread] Frame %d: capture_texture returned null\n", cap_frame);
            OutputDebugStringA(dbg);
        }

        // Sleep until next frame time; if we're behind, reset to now
        auto now = clock::now();
        if (next_frame > now) {
            std::this_thread::sleep_until(next_frame);
        } else {
            next_frame = now;
        }
    }

    {
        char dbg[128];
        snprintf(dbg, sizeof(dbg), "[CapThread] Exiting: running=%d connected=%d sharing=%d frames=%d\n",
                 (int)app.running.load(), (int)app.connected.load(), (int)app.screen_sharing.load(), cap_frame);
        OutputDebugStringA(dbg);
    }
    CoUninitialize();
}

// ── System audio capture thread (runs alongside screen sharing) ──
void sys_audio_capture_thread_func(AppState& app) {
    try {
        SystemAudioCapture capture;

        if (!capture.is_initialized()) {
            app.add_system_msg("System audio capture failed to initialize.");
            return;
        }
        if (capture.excludes_self()) {
            app.add_system_msg("System audio: capturing (LilyPad audio excluded).");
        } else {
            app.add_system_msg("System audio: fallback mode (LilyPad audio may be included).");
        }

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
        // System audio capture failed -- silently stop
    }
}
