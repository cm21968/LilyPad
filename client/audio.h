#pragma once

#include <portaudio.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace lilypad {

// ── Device info for UI selection ──
struct AudioDeviceInfo {
    int         index;
    std::string name;
    int         max_input_channels;
    int         max_output_channels;
};

// ── RAII PortAudio initializer ──
class PortAudioInit {
public:
    PortAudioInit();
    ~PortAudioInit();
    PortAudioInit(const PortAudioInit&) = delete;
    PortAudioInit& operator=(const PortAudioInit&) = delete;
};

// ── Device enumeration (call after PortAudioInit) ──
std::vector<AudioDeviceInfo> get_input_devices();
std::vector<AudioDeviceInfo> get_output_devices();
int get_default_input_device();
int get_default_output_device();

// ── Microphone capture (blocking read) ──
class AudioCapture {
public:
    // device_index = -1 means use default device
    AudioCapture(int sample_rate, int channels, int frame_size, int device_index = -1);
    ~AudioCapture();
    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    std::vector<float> read_frame();

private:
    PaStream* stream_ = nullptr;
    int       frame_size_;
    int       channels_;
};

// ── Speaker playback (blocking write) ──
class AudioPlayback {
public:
    // device_index = -1 means use default device
    AudioPlayback(int sample_rate, int channels, int frame_size, int device_index = -1);
    ~AudioPlayback();
    AudioPlayback(const AudioPlayback&) = delete;
    AudioPlayback& operator=(const AudioPlayback&) = delete;

    void write_frame(const std::vector<float>& pcm);

private:
    PaStream* stream_ = nullptr;
    int       frame_size_;
    int       channels_;
};

} // namespace lilypad
