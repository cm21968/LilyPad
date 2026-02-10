#pragma once

#include <vector>

class SystemAudioCapture {
public:
    SystemAudioCapture();   // initializes WASAPI loopback on default render device
    ~SystemAudioCapture();
    SystemAudioCapture(const SystemAudioCapture&) = delete;
    SystemAudioCapture& operator=(const SystemAudioCapture&) = delete;

    // Returns mono float32 samples at 48kHz. May return empty if no data ready.
    // Caller should call this in a loop with ~5-10ms sleep.
    std::vector<float> read_samples();

private:
    void*  device_       = nullptr;  // IMMDevice* (only used in fallback path)
    void*  audio_client_ = nullptr;  // IAudioClient*
    void*  capture_      = nullptr;  // IAudioCaptureClient*
    void*  mix_format_   = nullptr;  // WAVEFORMATEX*
    int    channels_     = 0;
    int    sample_rate_  = 0;
    bool   is_float_     = false;
    bool   initialized_  = false;
    bool   exclude_self_ = false;    // true if process-exclude loopback is active
};
