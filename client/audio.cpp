#include "audio.h"

namespace lilypad {

// ── PortAudioInit ──

PortAudioInit::PortAudioInit() {
    PaError err = Pa_Initialize();
    if (err != paNoError) {
        throw std::runtime_error("PortAudio init failed: " +
                                 std::string(Pa_GetErrorText(err)));
    }
}

PortAudioInit::~PortAudioInit() {
    Pa_Terminate();
}

// ── Device enumeration ──

std::vector<AudioDeviceInfo> get_input_devices() {
    std::vector<AudioDeviceInfo> devices;
    int count = Pa_GetDeviceCount();
    for (int i = 0; i < count; ++i) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (info && info->maxInputChannels > 0) {
            devices.push_back({i, info->name, info->maxInputChannels, info->maxOutputChannels});
        }
    }
    return devices;
}

std::vector<AudioDeviceInfo> get_output_devices() {
    std::vector<AudioDeviceInfo> devices;
    int count = Pa_GetDeviceCount();
    for (int i = 0; i < count; ++i) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (info && info->maxOutputChannels > 0) {
            devices.push_back({i, info->name, info->maxInputChannels, info->maxOutputChannels});
        }
    }
    return devices;
}

int get_default_input_device() {
    return Pa_GetDefaultInputDevice();
}

int get_default_output_device() {
    return Pa_GetDefaultOutputDevice();
}

// ── AudioCapture ──

AudioCapture::AudioCapture(int sample_rate, int channels, int frame_size, int device_index)
    : frame_size_(frame_size), channels_(channels) {

    PaStreamParameters params{};
    if (device_index >= 0) {
        params.device = device_index;
    } else {
        params.device = Pa_GetDefaultInputDevice();
    }
    if (params.device == paNoDevice) {
        throw std::runtime_error("No input device available");
    }
    params.channelCount = channels;
    params.sampleFormat = paFloat32;
    params.suggestedLatency = Pa_GetDeviceInfo(params.device)->defaultLowInputLatency;
    params.hostApiSpecificStreamInfo = nullptr;

    PaError err = Pa_OpenStream(
        &stream_,
        &params,        // input
        nullptr,        // no output
        sample_rate,
        frame_size,
        paClipOff,
        nullptr,        // blocking mode
        nullptr
    );
    if (err != paNoError) {
        throw std::runtime_error("Failed to open capture stream: " +
                                 std::string(Pa_GetErrorText(err)));
    }

    err = Pa_StartStream(stream_);
    if (err != paNoError) {
        Pa_CloseStream(stream_);
        throw std::runtime_error("Failed to start capture stream: " +
                                 std::string(Pa_GetErrorText(err)));
    }
}

AudioCapture::~AudioCapture() {
    if (stream_) {
        Pa_StopStream(stream_);
        Pa_CloseStream(stream_);
    }
}

std::vector<float> AudioCapture::read_frame() {
    std::vector<float> buf(frame_size_ * channels_);
    PaError err = Pa_ReadStream(stream_, buf.data(), frame_size_);
    if (err != paNoError && err != paInputOverflowed) {
        throw std::runtime_error("Capture read failed: " +
                                 std::string(Pa_GetErrorText(err)));
    }
    return buf;
}

// ── AudioPlayback ──

AudioPlayback::AudioPlayback(int sample_rate, int channels, int frame_size, int device_index)
    : frame_size_(frame_size), channels_(channels) {

    PaStreamParameters params{};
    if (device_index >= 0) {
        params.device = device_index;
    } else {
        params.device = Pa_GetDefaultOutputDevice();
    }
    if (params.device == paNoDevice) {
        throw std::runtime_error("No output device available");
    }
    params.channelCount = channels;
    params.sampleFormat = paFloat32;
    params.suggestedLatency = Pa_GetDeviceInfo(params.device)->defaultLowOutputLatency;
    params.hostApiSpecificStreamInfo = nullptr;

    PaError err = Pa_OpenStream(
        &stream_,
        nullptr,        // no input
        &params,        // output
        sample_rate,
        frame_size,
        paClipOff,
        nullptr,        // blocking mode
        nullptr
    );
    if (err != paNoError) {
        throw std::runtime_error("Failed to open playback stream: " +
                                 std::string(Pa_GetErrorText(err)));
    }

    err = Pa_StartStream(stream_);
    if (err != paNoError) {
        Pa_CloseStream(stream_);
        throw std::runtime_error("Failed to start playback stream: " +
                                 std::string(Pa_GetErrorText(err)));
    }
}

AudioPlayback::~AudioPlayback() {
    if (stream_) {
        Pa_StopStream(stream_);
        Pa_CloseStream(stream_);
    }
}

void AudioPlayback::write_frame(const std::vector<float>& pcm) {
    PaError err = Pa_WriteStream(stream_, pcm.data(), frame_size_);
    if (err != paNoError && err != paOutputUnderflowed) {
        throw std::runtime_error("Playback write failed: " +
                                 std::string(Pa_GetErrorText(err)));
    }
}

} // namespace lilypad
