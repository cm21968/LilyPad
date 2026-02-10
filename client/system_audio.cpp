#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "system_audio.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <functiondiscoverykeys_devpkey.h>

#include <cmath>
#include <cstring>

// WASAPI GUIDs
static const CLSID  CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
static const IID    IID_IMMDeviceEnumerator   = __uuidof(IMMDeviceEnumerator);
static const IID    IID_IAudioClient          = __uuidof(IAudioClient);
static const IID    IID_IAudioCaptureClient   = __uuidof(IAudioCaptureClient);

// Helper casts from opaque void* members to real COM types
#define DEV()      static_cast<IMMDevice*>(device_)
#define CLIENT()   static_cast<IAudioClient*>(audio_client_)
#define CAP()      static_cast<IAudioCaptureClient*>(capture_)
#define FMT()      static_cast<WAVEFORMATEX*>(mix_format_)

// ── Minimal IActivateAudioInterfaceCompletionHandler for synchronous wait ──
class ActivateAudioInterfaceHandler : public IActivateAudioInterfaceCompletionHandler {
public:
    ActivateAudioInterfaceHandler() {
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    ~ActivateAudioInterfaceHandler() {
        if (event_) CloseHandle(event_);
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override  { return InterlockedIncrement(&ref_count_); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG count = InterlockedDecrement(&ref_count_);
        if (count == 0) delete this;
        return count;
    }

    // IActivateAudioInterfaceCompletionHandler
    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override {
        op_ = op;
        op_->AddRef();
        SetEvent(event_);
        return S_OK;
    }

    HRESULT Wait(DWORD timeout_ms = 3000) {
        if (WaitForSingleObject(event_, timeout_ms) != WAIT_OBJECT_0)
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        return S_OK;
    }

    HRESULT GetResult(IAudioClient** client) {
        if (!op_) return E_FAIL;
        HRESULT hr_activate = S_OK;
        IUnknown* punk = nullptr;
        HRESULT hr = op_->GetActivateResult(&hr_activate, &punk);
        if (FAILED(hr)) return hr;
        if (FAILED(hr_activate)) return hr_activate;
        if (!punk) return E_FAIL;
        hr = punk->QueryInterface(IID_IAudioClient, reinterpret_cast<void**>(client));
        punk->Release();
        return hr;
    }

private:
    HANDLE event_ = nullptr;
    IActivateAudioInterfaceAsyncOperation* op_ = nullptr;
    volatile ULONG ref_count_ = 1;
};

// ── Helper: initialize IAudioClient for capture and start ──
static bool init_client_for_capture(IAudioClient* client, WAVEFORMATEX** out_fmt,
                                     IAudioCaptureClient** out_cap,
                                     int& channels, int& sample_rate, bool& is_float,
                                     DWORD stream_flags) {
    WAVEFORMATEX* fmt = nullptr;
    HRESULT hr = client->GetMixFormat(&fmt);
    if (FAILED(hr) || !fmt) return false;
    *out_fmt = fmt;

    channels    = fmt->nChannels;
    sample_rate = static_cast<int>(fmt->nSamplesPerSec);

    if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        is_float = true;
    } else if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(fmt);
        is_float = (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    }

    REFERENCE_TIME buffer_duration = 400000; // 40ms in 100ns units
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, stream_flags,
                            buffer_duration, 0, fmt, nullptr);
    if (FAILED(hr)) return false;

    hr = client->GetService(IID_IAudioCaptureClient, reinterpret_cast<void**>(out_cap));
    if (FAILED(hr) || !*out_cap) return false;

    hr = client->Start();
    if (FAILED(hr)) return false;

    return true;
}

SystemAudioCapture::SystemAudioCapture() {
    HRESULT hr;

    hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != S_FALSE) return;

    // ── Path 1: Process-exclude loopback (Windows 10 2004+ / build 19041+) ──
    // Captures all system audio EXCEPT this process's own audio output.
    {
        AUDIOCLIENT_ACTIVATION_PARAMS ac_params{};
        ac_params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        ac_params.ProcessLoopbackParams.TargetProcessId = GetCurrentProcessId();
        ac_params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;

        PROPVARIANT activate_params{};
        activate_params.vt = VT_BLOB;
        activate_params.blob.cbSize = sizeof(ac_params);
        activate_params.blob.pBlobData = reinterpret_cast<BYTE*>(&ac_params);

        auto* handler = new ActivateAudioInterfaceHandler();
        IActivateAudioInterfaceAsyncOperation* async_op = nullptr;

        hr = ActivateAudioInterfaceAsync(
            VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
            IID_IAudioClient,
            &activate_params,
            handler,
            &async_op);

        if (SUCCEEDED(hr) && SUCCEEDED(handler->Wait())) {
            IAudioClient* client = nullptr;
            if (SUCCEEDED(handler->GetResult(&client)) && client) {
                IAudioCaptureClient* cap = nullptr;
                WAVEFORMATEX* fmt = nullptr;
                // Stream flags are 0 — loopback mode is specified in activation params
                if (init_client_for_capture(client, &fmt, &cap,
                                            channels_, sample_rate_, is_float_, 0)) {
                    audio_client_ = client;
                    capture_      = cap;
                    mix_format_   = fmt;
                    exclude_self_ = true;
                    initialized_  = true;
                } else {
                    if (fmt) CoTaskMemFree(fmt);
                    client->Release();
                }
            }
        }

        if (async_op) async_op->Release();
        handler->Release();
    }

    // ── Path 2: Standard loopback fallback (older Windows) ──
    if (!initialized_) {
        IMMDeviceEnumerator* enumerator = nullptr;
        hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                              IID_IMMDeviceEnumerator, reinterpret_cast<void**>(&enumerator));
        if (FAILED(hr) || !enumerator) return;

        IMMDevice* dev = nullptr;
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
        enumerator->Release();
        if (FAILED(hr) || !dev) return;
        device_ = dev;

        IAudioClient* client = nullptr;
        hr = dev->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr,
                           reinterpret_cast<void**>(&client));
        if (FAILED(hr) || !client) return;

        IAudioCaptureClient* cap = nullptr;
        WAVEFORMATEX* fmt = nullptr;
        if (init_client_for_capture(client, &fmt, &cap,
                                    channels_, sample_rate_, is_float_,
                                    AUDCLNT_STREAMFLAGS_LOOPBACK)) {
            audio_client_ = client;
            capture_      = cap;
            mix_format_   = fmt;
            exclude_self_ = false;
            initialized_  = true;
        } else {
            if (fmt) CoTaskMemFree(fmt);
            client->Release();
        }
    }
}

SystemAudioCapture::~SystemAudioCapture() {
    if (audio_client_) CLIENT()->Stop();
    if (capture_)      CAP()->Release();
    if (audio_client_) CLIENT()->Release();
    if (device_)       DEV()->Release();
    if (mix_format_)   CoTaskMemFree(mix_format_);
}

std::vector<float> SystemAudioCapture::read_samples() {
    std::vector<float> result;
    if (!initialized_ || !capture_) return result;

    auto* cap = CAP();

    UINT32 packet_length = 0;
    HRESULT hr = cap->GetNextPacketSize(&packet_length);
    if (FAILED(hr)) return result;

    while (packet_length > 0) {
        BYTE* data = nullptr;
        UINT32 num_frames = 0;
        DWORD flags = 0;

        hr = cap->GetBuffer(&data, &num_frames, &flags, nullptr, nullptr);
        if (FAILED(hr)) break;

        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            // Device is outputting silence — produce silent mono samples
            result.insert(result.end(), num_frames, 0.0f);
        } else if (data && num_frames > 0) {
            // Convert to mono float
            for (UINT32 i = 0; i < num_frames; ++i) {
                float sample = 0.0f;
                if (is_float_) {
                    const float* fdata = reinterpret_cast<const float*>(data);
                    // Average all channels to mono
                    for (int ch = 0; ch < channels_; ++ch) {
                        sample += fdata[i * channels_ + ch];
                    }
                    sample /= static_cast<float>(channels_);
                } else {
                    // 16-bit PCM fallback
                    const int16_t* sdata = reinterpret_cast<const int16_t*>(data);
                    for (int ch = 0; ch < channels_; ++ch) {
                        sample += static_cast<float>(sdata[i * channels_ + ch]) / 32768.0f;
                    }
                    sample /= static_cast<float>(channels_);
                }
                result.push_back(sample);
            }
        }

        cap->ReleaseBuffer(num_frames);

        hr = cap->GetNextPacketSize(&packet_length);
        if (FAILED(hr)) break;
    }

    return result;
}
