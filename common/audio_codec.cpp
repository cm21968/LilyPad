#include "audio_codec.h"

namespace lilypad {

// ── OpusEncoderWrapper ──

OpusEncoderWrapper::OpusEncoderWrapper() {
    int error = 0;
    encoder_ = opus_encoder_create(SAMPLE_RATE, CHANNELS, OPUS_APPLICATION_VOIP, &error);
    if (error != OPUS_OK || !encoder_) {
        throw std::runtime_error("Failed to create Opus encoder: " +
                                 std::string(opus_strerror(error)));
    }
    opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(BITRATE));
    opus_encoder_ctl(encoder_, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
}

OpusEncoderWrapper::~OpusEncoderWrapper() {
    if (encoder_) {
        opus_encoder_destroy(encoder_);
    }
}

std::vector<uint8_t> OpusEncoderWrapper::encode(const float* pcm, int frame_size) {
    std::vector<uint8_t> out(MAX_OPUS_PACKET);
    int encoded = opus_encode_float(encoder_, pcm, frame_size,
                                    out.data(), static_cast<opus_int32>(out.size()));
    if (encoded < 0) {
        throw std::runtime_error("Opus encode failed: " +
                                 std::string(opus_strerror(encoded)));
    }
    out.resize(static_cast<size_t>(encoded));
    return out;
}

// ── OpusDecoderWrapper ──

OpusDecoderWrapper::OpusDecoderWrapper() {
    int error = 0;
    decoder_ = opus_decoder_create(SAMPLE_RATE, CHANNELS, &error);
    if (error != OPUS_OK || !decoder_) {
        throw std::runtime_error("Failed to create Opus decoder: " +
                                 std::string(opus_strerror(error)));
    }
}

OpusDecoderWrapper::~OpusDecoderWrapper() {
    if (decoder_) {
        opus_decoder_destroy(decoder_);
    }
}

std::vector<float> OpusDecoderWrapper::decode(const uint8_t* data, int len, int frame_size) {
    std::vector<float> pcm(frame_size * CHANNELS);
    int decoded = opus_decode_float(decoder_, data, len,
                                    pcm.data(), frame_size, 0);
    if (decoded < 0) {
        throw std::runtime_error("Opus decode failed: " +
                                 std::string(opus_strerror(decoded)));
    }
    pcm.resize(static_cast<size_t>(decoded * CHANNELS));
    return pcm;
}

std::vector<float> OpusDecoderWrapper::decode_plc(int frame_size) {
    std::vector<float> pcm(frame_size * CHANNELS);
    int decoded = opus_decode_float(decoder_, nullptr, 0,
                                    pcm.data(), frame_size, 0);
    if (decoded < 0) {
        // PLC failed — return silence
        std::fill(pcm.begin(), pcm.end(), 0.0f);
        return pcm;
    }
    pcm.resize(static_cast<size_t>(decoded * CHANNELS));
    return pcm;
}

} // namespace lilypad
