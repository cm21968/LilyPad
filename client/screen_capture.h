#pragma once

#include <cstdint>
#include <vector>

namespace lilypad {

// Screen sharing resolution presets
enum class ScreenResolution : int {
    RES_720p  = 0,  // 1280x720
    RES_1080p = 1,  // 1920x1080
};

// Screen sharing FPS presets
enum class ScreenFps : int {
    FPS_30 = 0,
    FPS_60 = 1,
};

// Get target dimensions for a resolution preset
inline void get_capture_dimensions(ScreenResolution res, int& w, int& h) {
    switch (res) {
    case ScreenResolution::RES_1080p: w = 1920; h = 1080; break;
    case ScreenResolution::RES_720p:
    default:                          w = 1280; h = 720;  break;
    }
}

// Get sleep interval in milliseconds for an FPS preset
inline int get_capture_interval_ms(ScreenFps fps) {
    switch (fps) {
    case ScreenFps::FPS_60: return 16;
    case ScreenFps::FPS_30:
    default:                return 33;
    }
}

// Persistent screen capturer — uses DXGI Desktop Duplication (GPU-accelerated).
// Falls back to GDI if DXGI is unavailable.
class ScreenCapturer {
public:
    ScreenCapturer();
    ~ScreenCapturer();
    ScreenCapturer(const ScreenCapturer&) = delete;
    ScreenCapturer& operator=(const ScreenCapturer&) = delete;

    // Capture screen as JPEG.
    std::vector<uint8_t> capture_jpeg(int quality, int target_w, int target_h,
                                       int& out_width, int& out_height);

private:
    void* impl_ = nullptr;
    bool init_dxgi();
    void release_dxgi();
};

// Legacy standalone capture (GDI-based, kept as internal fallback).
std::vector<uint8_t> capture_screen_jpeg(int quality, int target_w, int target_h,
                                          int& out_width, int& out_height);

// Decode JPEG data to RGBA pixels.
// Returns RGBA pixel data, sets out_width/out_height.
// Returns empty vector on failure.
std::vector<uint8_t> decode_jpeg_to_rgba(const uint8_t* jpeg_data, size_t jpeg_len,
                                          int& out_width, int& out_height);

} // namespace lilypad
