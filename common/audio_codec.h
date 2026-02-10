#pragma once

#include <opus/opus.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

namespace lilypad {

constexpr int SAMPLE_RATE = 48000;
constexpr int CHANNELS    = 1;
constexpr int FRAME_SIZE  = 960;   // 20ms at 48kHz
constexpr int BITRATE     = 64000;

// Maximum encoded frame size in bytes
constexpr int MAX_OPUS_PACKET = 4000;

// ── Opus encoder wrapper (RAII) ──
class OpusEncoderWrapper {
public:
    OpusEncoderWrapper();
    ~OpusEncoderWrapper();
    OpusEncoderWrapper(const OpusEncoderWrapper&) = delete;
    OpusEncoderWrapper& operator=(const OpusEncoderWrapper&) = delete;
    OpusEncoderWrapper(OpusEncoderWrapper&& other) noexcept : encoder_(other.encoder_) { other.encoder_ = nullptr; }
    OpusEncoderWrapper& operator=(OpusEncoderWrapper&& other) noexcept {
        if (this != &other) { if (encoder_) opus_encoder_destroy(encoder_); encoder_ = other.encoder_; other.encoder_ = nullptr; }
        return *this;
    }

    // Encode one frame of PCM float samples. Returns Opus-encoded bytes.
    std::vector<uint8_t> encode(const float* pcm, int frame_size = FRAME_SIZE);

private:
    OpusEncoder* encoder_ = nullptr;
};

// ── Opus decoder wrapper (RAII) ──
class OpusDecoderWrapper {
public:
    OpusDecoderWrapper();
    ~OpusDecoderWrapper();
    OpusDecoderWrapper(const OpusDecoderWrapper&) = delete;
    OpusDecoderWrapper& operator=(const OpusDecoderWrapper&) = delete;
    OpusDecoderWrapper(OpusDecoderWrapper&& other) noexcept : decoder_(other.decoder_) { other.decoder_ = nullptr; }
    OpusDecoderWrapper& operator=(OpusDecoderWrapper&& other) noexcept {
        if (this != &other) { if (decoder_) opus_decoder_destroy(decoder_); decoder_ = other.decoder_; other.decoder_ = nullptr; }
        return *this;
    }

    // Decode Opus bytes into PCM float samples.
    std::vector<float> decode(const uint8_t* data, int len, int frame_size = FRAME_SIZE);

    // Packet Loss Concealment: generate interpolated audio when a packet is missing.
    std::vector<float> decode_plc(int frame_size = FRAME_SIZE);

private:
    OpusDecoder* decoder_ = nullptr;
};

} // namespace lilypad
