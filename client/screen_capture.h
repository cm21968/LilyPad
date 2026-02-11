#pragma once

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

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
// Returns a D3D11_USAGE_DEFAULT texture suitable for direct use by the MF encoder.
class ScreenCapturer {
public:
    ScreenCapturer();
    ~ScreenCapturer();
    ScreenCapturer(const ScreenCapturer&) = delete;
    ScreenCapturer& operator=(const ScreenCapturer&) = delete;

    // Capture screen as a GPU texture (D3D11_USAGE_DEFAULT).
    // Returns the texture pointer (owned by this class, valid until next call or destruction).
    // Returns nullptr if no new frame is available.
    ID3D11Texture2D* capture_texture(int& out_width, int& out_height);

    // Access to the D3D11 device/context used for capture (needed by H.264 encoder)
    ID3D11Device* get_device() const;
    ID3D11DeviceContext* get_context() const;

    // Native screen dimensions
    int screen_width() const;
    int screen_height() const;

private:
    void* impl_ = nullptr;
    bool init_dxgi();
    void release_dxgi();
};

} // namespace lilypad
