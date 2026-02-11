#pragma once

#include <cstdint>
#include <vector>

struct ID3D11Device;
struct ID3D11Texture2D;

namespace lilypad {

class H264Encoder {
public:
    H264Encoder();
    ~H264Encoder();
    H264Encoder(const H264Encoder&) = delete;
    H264Encoder& operator=(const H264Encoder&) = delete;

    // Initialize encoder for given output resolution and framerate.
    // device: the D3D11 device from screen capture (encoder shares it via DXGI device manager).
    bool init(ID3D11Device* device, int width, int height, int fps, int bitrate_bps);
    void shutdown();

    // Encode a BGRA texture to H.264 NAL units.
    // Input texture must be D3D11_USAGE_DEFAULT on the same device passed to init().
    // force_idr: request an IDR keyframe.
    // is_keyframe: set to true if the output is an IDR.
    // Returns empty vector on failure or if encoder needs more input.
    std::vector<uint8_t> encode(ID3D11Texture2D* texture, bool force_idr, bool& is_keyframe);

    // Change target bitrate on the fly.
    void set_bitrate(int bitrate_bps);

    bool is_initialized() const;

private:
    void* impl_ = nullptr;
};

} // namespace lilypad
