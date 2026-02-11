#pragma once

#include <cstdint>
#include <cstddef>

struct ID3D11Device;
struct ID3D11ShaderResourceView;

namespace lilypad {

class H264Decoder {
public:
    H264Decoder();
    ~H264Decoder();
    H264Decoder(const H264Decoder&) = delete;
    H264Decoder& operator=(const H264Decoder&) = delete;

    // Initialize decoder.
    // device: the main rendering D3D11 device (g_d3d_device) so output can be used as SRV directly.
    bool init(ID3D11Device* device);
    void shutdown();

    // Decode an H.264 frame. Returns true if a new output frame is available.
    bool decode(const uint8_t* data, size_t len, bool is_keyframe);

    // Get the output SRV (RGBA texture). Valid after a successful decode().
    // The SRV is owned by this class and valid until shutdown() or a resolution change.
    ID3D11ShaderResourceView* get_output_srv() const;

    int width() const;
    int height() const;

    // Flush decoder pipeline (call when stopping playback).
    void flush();

    bool is_initialized() const;

private:
    void* impl_ = nullptr;
};

} // namespace lilypad
