#include "h264_encoder.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
#include <initguid.h>
#include <wmcodecdsp.h>

#include <algorithm>
#include <cstdio>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

namespace lilypad {

// ════════════════════════════════════════════════════════════════
//  H.264 Encoder Implementation
//
//  Tries hardware encoder with DXGI device manager first.
//  Falls back to software encoder (Microsoft H264 Encoder MFT)
//  with CPU readback + software BGRA→NV12 conversion.
// ════════════════════════════════════════════════════════════════

struct H264EncoderImpl {
    IMFTransform*          encoder         = nullptr;  // H.264 encoder MFT
    IMFDXGIDeviceManager*  device_manager  = nullptr;  // null in software mode
    IMFTransform*          color_converter = nullptr;  // Video Processor MFT (GPU mode only)
    IMFMediaEventGenerator* event_gen      = nullptr;  // non-null for async MFTs
    UINT                   reset_token     = 0;
    ID3D11Device*          device          = nullptr;  // not owned
    ID3D11DeviceContext*   context         = nullptr;  // not owned
    ID3D11Texture2D*       staging         = nullptr;  // staging texture for CPU readback (software mode)
    int                    width           = 0;
    int                    height          = 0;
    int                    fps             = 30;
    bool                   initialized     = false;
    bool                   software_mode   = false;    // true = CPU readback path
    bool                   is_async        = false;    // true = async MFT (NVENC)
    int                    pending_need_input = 0;     // buffered METransformNeedInput count
    LONGLONG               sample_time     = 0;
    LONGLONG               sample_duration = 0;
    int                    frame_count     = 0;        // frames encoded (reset per init)
    int                    output_count    = 0;        // successful outputs (reset per init)
    int                    gop_size        = 60;       // frames between keyframes
};

// ── Software BGRA→NV12 conversion ──
// BGRA layout: B G R A per pixel, row pitch may differ from width*4
// NV12 layout: Y plane (w*h bytes), then interleaved UV plane (w*h/2 bytes)
static void bgra_to_nv12(const uint8_t* bgra, int src_pitch, int w, int h, uint8_t* nv12) {
    uint8_t* y_plane  = nv12;
    uint8_t* uv_plane = nv12 + w * h;

    for (int row = 0; row < h; row++) {
        const uint8_t* src_row = bgra + row * src_pitch;
        for (int col = 0; col < w; col++) {
            int b = src_row[col * 4 + 0];
            int g = src_row[col * 4 + 1];
            int r = src_row[col * 4 + 2];

            // BT.601 RGB→YUV
            int y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
            y_plane[row * w + col] = static_cast<uint8_t>((std::min)((std::max)(y, 0), 255));

            // Subsample UV at 2x2 blocks
            if ((row & 1) == 0 && (col & 1) == 0) {
                int u = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
                int v = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
                int uv_idx = (row / 2) * w + col;
                uv_plane[uv_idx + 0] = static_cast<uint8_t>((std::min)((std::max)(u, 0), 255));
                uv_plane[uv_idx + 1] = static_cast<uint8_t>((std::min)((std::max)(v, 0), 255));
            }
        }
    }
}

// Helper: configure codec settings and start streaming on an encoder MFT
static void configure_and_start_encoder(IMFTransform* encoder, int bitrate, int fps) {
    ICodecAPI* codec = nullptr;
    HRESULT hr = encoder->QueryInterface(IID_PPV_ARGS(&codec));
    if (SUCCEEDED(hr) && codec) {
        VARIANT var;
        VariantInit(&var);

        // Low latency for real-time streaming
        var.vt = VT_BOOL; var.boolVal = VARIANT_TRUE;
        codec->SetValue(&CODECAPI_AVLowLatencyMode, &var);

        // CBR rate control
        var.vt = VT_UI4; var.ulVal = eAVEncCommonRateControlMode_CBR;
        codec->SetValue(&CODECAPI_AVEncCommonRateControlMode, &var);

        // Bitrate
        var.vt = VT_UI4; var.ulVal = bitrate;
        codec->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &var);

        // GOP size — keyframe every 2 seconds
        var.vt = VT_UI4; var.ulVal = fps * 2;
        codec->SetValue(&CODECAPI_AVEncMPVGOPSize, &var);

        // Quality vs speed — favor quality (100 = max quality, 0 = max speed)
        var.vt = VT_UI4; var.ulVal = 70;
        codec->SetValue(&CODECAPI_AVEncCommonQualityVsSpeed, &var);

        // Max bitrate = 1.5x mean (allows headroom for complex scenes)
        var.vt = VT_UI4; var.ulVal = bitrate * 3 / 2;
        codec->SetValue(&CODECAPI_AVEncCommonMaxBitRate, &var);

        codec->Release();
    }
    encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
}

// Helper: set output + input type on an encoder. Returns true on success.
// out_accepts_argb: set to true if encoder accepts ARGB32 input (no color converter needed)
static bool set_encoder_types(IMFTransform* encoder, int w, int h, int fps, int bitrate,
                               bool& out_accepts_argb) {
    out_accepts_argb = false;

    IMFMediaType* out_mt = nullptr;
    MFCreateMediaType(&out_mt);
    out_mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out_mt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    out_mt->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    MFSetAttributeSize(out_mt, MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(out_mt, MF_MT_FRAME_RATE, fps, 1);
    out_mt->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    out_mt->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    HRESULT hr = encoder->SetOutputType(0, out_mt, 0);
    out_mt->Release();
    if (FAILED(hr)) {
        char msg[128]; snprintf(msg, sizeof(msg), "[H264Enc] SetOutputType failed hr=0x%08lX\n", hr);
        OutputDebugStringA(msg);
        return false;
    }

    // Try ARGB32 first — allows direct BGRA texture input (no color converter needed)
    for (DWORD i = 0; ; i++) {
        IMFMediaType* avail = nullptr;
        if (FAILED(encoder->GetInputAvailableType(0, i, &avail))) break;
        GUID sub{}; avail->GetGUID(MF_MT_SUBTYPE, &sub);
        if (sub == MFVideoFormat_ARGB32) {
            MFSetAttributeSize(avail, MF_MT_FRAME_SIZE, w, h);
            MFSetAttributeRatio(avail, MF_MT_FRAME_RATE, fps, 1);
            hr = encoder->SetInputType(0, avail, 0);
            avail->Release();
            if (SUCCEEDED(hr)) {
                out_accepts_argb = true;
                OutputDebugStringA("[H264Enc] Encoder accepts ARGB32 input (no color converter needed)\n");
                return true;
            }
        } else {
            avail->Release();
        }
    }

    // Try NV12 from enumerated types
    for (DWORD i = 0; ; i++) {
        IMFMediaType* avail = nullptr;
        if (FAILED(encoder->GetInputAvailableType(0, i, &avail))) break;
        GUID sub{}; avail->GetGUID(MF_MT_SUBTYPE, &sub);
        if (sub == MFVideoFormat_NV12) {
            MFSetAttributeSize(avail, MF_MT_FRAME_SIZE, w, h);
            MFSetAttributeRatio(avail, MF_MT_FRAME_RATE, fps, 1);
            hr = encoder->SetInputType(0, avail, 0);
            avail->Release();
            if (SUCCEEDED(hr)) {
                OutputDebugStringA("[H264Enc] Encoder accepts NV12 input\n");
                return true;
            }
        } else {
            avail->Release();
        }
    }

    // Manual NV12
    IMFMediaType* in_mt = nullptr;
    MFCreateMediaType(&in_mt);
    in_mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in_mt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(in_mt, MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(in_mt, MF_MT_FRAME_RATE, fps, 1);
    in_mt->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    hr = encoder->SetInputType(0, in_mt, 0);
    in_mt->Release();
    if (SUCCEEDED(hr)) OutputDebugStringA("[H264Enc] Encoder accepts NV12 input (manual)\n");
    return SUCCEEDED(hr);
}

// Helper: create a D3D device + device manager from an MFT's adapter LUID
static IMFDXGIDeviceManager* create_dm_for_activate(IMFActivate* activate,
                                                     ID3D11Device** out_device) {
    *out_device = nullptr;

    // Try to get the adapter LUID the MFT wants; if not available,
    // use the first adapter that has a display output (likely the GPU)
    IDXGIFactory1* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
    if (FAILED(hr)) return nullptr;

    IDXGIAdapter* target_adapter = nullptr;
    UINT64 luid_val = 0;
    bool has_luid = SUCCEEDED(activate->GetUINT64(MFT_ENUM_ADAPTER_LUID, &luid_val));

    for (UINT i = 0; ; i++) {
        IDXGIAdapter* adapter = nullptr;
        if (factory->EnumAdapters(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC desc{};
        adapter->GetDesc(&desc);

        bool match = false;
        if (has_luid) {
            LUID want;
            want.LowPart  = static_cast<DWORD>(luid_val & 0xFFFFFFFF);
            want.HighPart = static_cast<LONG>(luid_val >> 32);
            match = (desc.AdapterLuid.LowPart == want.LowPart &&
                     desc.AdapterLuid.HighPart == want.HighPart);
        } else {
            // No LUID — pick the first adapter with a display output
            IDXGIOutput* out = nullptr;
            if (SUCCEEDED(adapter->EnumOutputs(0, &out))) {
                match = true;
                out->Release();
            }
        }

        if (match) {
            target_adapter = adapter;
            char name[128];
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
            char msg[256];
            snprintf(msg, sizeof(msg), "[H264Enc] Creating dedicated device on: %s\n", name);
            OutputDebugStringA(msg);
            break;
        }
        adapter->Release();
    }
    factory->Release();

    if (!target_adapter) return nullptr;

    // Create a D3D device on this specific adapter
    D3D_FEATURE_LEVEL level;
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    hr = D3D11CreateDevice(target_adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                           D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                           levels, 1, D3D11_SDK_VERSION,
                           &device, &level, &ctx);
    target_adapter->Release();
    if (ctx) ctx->Release();  // encoder manages its own context
    if (FAILED(hr) || !device) return nullptr;

    // Enable multithread protection
    ID3D11Multithread* mt = nullptr;
    if (SUCCEEDED(device->QueryInterface(__uuidof(ID3D11Multithread),
                                         reinterpret_cast<void**>(&mt)))) {
        mt->SetMultithreadProtected(TRUE);
        mt->Release();
    }

    // Create device manager
    UINT token = 0;
    IMFDXGIDeviceManager* dm = nullptr;
    hr = MFCreateDXGIDeviceManager(&token, &dm);
    if (FAILED(hr)) { device->Release(); return nullptr; }

    hr = dm->ResetDevice(device, token);
    if (FAILED(hr)) { dm->Release(); device->Release(); return nullptr; }

    *out_device = device;
    return dm;
}

// ── Create and configure an H.264 encoder MFT ──
// out_hw_device: if hardware succeeds, set to the encoder's own D3D device (caller must Release)
static IMFTransform* create_h264_encoder(IMFDXGIDeviceManager* dm, int w, int h, int fps,
                                          int bitrate, bool& out_is_software,
                                          bool& out_is_async, bool& out_accepts_argb,
                                          IMFDXGIDeviceManager** out_hw_dm,
                                          ID3D11Device** out_hw_device) {
    out_is_software = false;
    out_accepts_argb = false;
    out_is_async = false;
    if (out_hw_dm) *out_hw_dm = nullptr;
    if (out_hw_device) *out_hw_device = nullptr;

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    MFT_REGISTER_TYPE_INFO output_type_info = { MFMediaType_Video, MFVideoFormat_H264 };
    HRESULT hr;

    // ── Try all hardware encoders ──
    hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                   MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                   nullptr, &output_type_info, &activates, &count);

    if (SUCCEEDED(hr) && count > 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "[H264Enc] Found %u hardware encoder(s)\n", count);
        OutputDebugStringA(msg);

        for (UINT32 idx = 0; idx < count; idx++) {
            IMFTransform* enc = nullptr;
            hr = activates[idx]->ActivateObject(IID_PPV_ARGS(&enc));
            if (FAILED(hr)) continue;

            // Check if async MFT — must unlock before any messages
            IMFAttributes* attrs = nullptr;
            bool is_async = false;
            bool d3d_aware = false;
            if (SUCCEEDED(enc->GetAttributes(&attrs)) && attrs) {
                UINT32 async_val = 0;
                if (SUCCEEDED(attrs->GetUINT32(MF_TRANSFORM_ASYNC, &async_val)) && async_val) {
                    is_async = true;
                    attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
                    OutputDebugStringA("[H264Enc] Async MFT detected, unlocked\n");
                }
                UINT32 d3d_val = 0;
                attrs->GetUINT32(MF_SA_D3D11_AWARE, &d3d_val);
                d3d_aware = (d3d_val != 0);
                attrs->Release();
            }

            // Try 1: use the caller's device manager (capture device)
            bool dm_ok = false;
            HRESULT dm_hr = E_FAIL;
            if (dm && d3d_aware) {
                dm_hr = enc->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                            reinterpret_cast<ULONG_PTR>(dm));
                dm_ok = SUCCEEDED(dm_hr);
                snprintf(msg, sizeof(msg), "[H264Enc] HW #%u SET_D3D shared: hr=0x%08lX\n", idx, dm_hr);
                OutputDebugStringA(msg);
            }

            if (dm_ok && set_encoder_types(enc, w, h, fps, bitrate, out_accepts_argb)) {
                snprintf(msg, sizeof(msg), "[H264Enc] Hardware encoder #%u ready (shared device, GPU path)\n", idx);
                OutputDebugStringA(msg);
                out_is_async = is_async;
                configure_and_start_encoder(enc, bitrate, fps);
                for (UINT32 j = 0; j < count; j++) activates[j]->Release();
                CoTaskMemFree(activates);
                return enc;
            }

            // Try 2: create a dedicated D3D device for the encoder
            if (!dm_ok && d3d_aware) {
                snprintf(msg, sizeof(msg),
                         "[H264Enc] HW #%u shared device failed (hr=0x%08lX), trying dedicated device\n",
                         idx, dm_hr);
                OutputDebugStringA(msg);

                // Reactivate for clean state
                enc->Release();
                enc = nullptr;
                activates[idx]->ShutdownObject();
                hr = activates[idx]->ActivateObject(IID_PPV_ARGS(&enc));
                if (FAILED(hr)) continue;

                // Re-unlock async if needed
                if (is_async) {
                    if (SUCCEEDED(enc->GetAttributes(&attrs)) && attrs) {
                        attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
                        attrs->Release();
                    }
                }

                ID3D11Device* hw_device = nullptr;
                IMFDXGIDeviceManager* hw_dm = create_dm_for_activate(activates[idx], &hw_device);
                if (hw_dm) {
                    dm_hr = enc->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                                reinterpret_cast<ULONG_PTR>(hw_dm));
                    dm_ok = SUCCEEDED(dm_hr);
                    snprintf(msg, sizeof(msg), "[H264Enc] HW #%u SET_D3D dedicated: hr=0x%08lX\n", idx, dm_hr);
                    OutputDebugStringA(msg);

                    if (dm_ok && set_encoder_types(enc, w, h, fps, bitrate, out_accepts_argb)) {
                        snprintf(msg, sizeof(msg), "[H264Enc] Hardware encoder #%u ready (dedicated device, GPU path)\n", idx);
                        OutputDebugStringA(msg);
                        out_is_async = is_async;
                        configure_and_start_encoder(enc, bitrate, fps);
                        if (out_hw_dm) *out_hw_dm = hw_dm; else hw_dm->Release();
                        if (out_hw_device) *out_hw_device = hw_device; else hw_device->Release();
                        for (UINT32 j = 0; j < count; j++) activates[j]->Release();
                        CoTaskMemFree(activates);
                        return enc;
                    }

                    snprintf(msg, sizeof(msg),
                             "[H264Enc] HW #%u dedicated device also failed (hr=0x%08lX)\n", idx, dm_hr);
                    OutputDebugStringA(msg);
                    hw_dm->Release();
                    hw_device->Release();
                }

                enc->Release();
                enc = nullptr;
            }

            // Try 3: hardware encoder WITHOUT device manager (CPU NV12 input, NVENC encoding)
            // Some NVIDIA MFTs reject external D3D managers but still use NVENC internally
            if (!dm_ok) {
                if (!enc) {
                    // Re-activate if we released above
                    activates[idx]->ShutdownObject();
                    hr = activates[idx]->ActivateObject(IID_PPV_ARGS(&enc));
                    if (FAILED(hr)) continue;
                    if (is_async) {
                        if (SUCCEEDED(enc->GetAttributes(&attrs)) && attrs) {
                            attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
                            attrs->Release();
                        }
                    }
                }

                if (set_encoder_types(enc, w, h, fps, bitrate, out_accepts_argb)) {
                    snprintf(msg, sizeof(msg),
                             "[H264Enc] Hardware encoder #%u ready (no D3D manager, CPU input)\n", idx);
                    OutputDebugStringA(msg);
                    out_is_software = true;  // use CPU readback path for input
                    configure_and_start_encoder(enc, bitrate, fps);
                    for (UINT32 j = 0; j < count; j++) activates[j]->Release();
                    CoTaskMemFree(activates);
                    return enc;
                }
                snprintf(msg, sizeof(msg), "[H264Enc] HW #%u type negotiation without D3D also failed\n", idx);
                OutputDebugStringA(msg);
                enc->Release();
            }
        }

        for (UINT32 i = 0; i < count; i++) activates[i]->Release();
        CoTaskMemFree(activates);
        activates = nullptr;
        OutputDebugStringA("[H264Enc] All hardware encoders failed, trying software\n");
    } else {
        if (activates) { CoTaskMemFree(activates); activates = nullptr; }
    }

    // ── Software encoder ──
    hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                   MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                   nullptr, &output_type_info, &activates, &count);
    if (FAILED(hr) || count == 0) {
        OutputDebugStringA("[H264Enc] No software H.264 encoder found\n");
        if (activates) CoTaskMemFree(activates);
        return nullptr;
    }

    IMFTransform* enc = nullptr;
    hr = activates[0]->ActivateObject(IID_PPV_ARGS(&enc));
    for (UINT32 i = 0; i < count; i++) activates[i]->Release();
    CoTaskMemFree(activates);
    if (FAILED(hr)) {
        char msg[128]; snprintf(msg, sizeof(msg), "[H264Enc] SW ActivateObject failed hr=0x%08lX\n", hr);
        OutputDebugStringA(msg);
        return nullptr;
    }

    if (!set_encoder_types(enc, w, h, fps, bitrate, out_accepts_argb)) {
        OutputDebugStringA("[H264Enc] SW encoder: type negotiation failed\n");
        enc->Release();
        return nullptr;
    }

    out_is_software = true;
    OutputDebugStringA("[H264Enc] Software encoder ready\n");
    configure_and_start_encoder(enc, bitrate, fps);
    return enc;
}

H264Encoder::H264Encoder() {
    impl_ = new H264EncoderImpl();
}

H264Encoder::~H264Encoder() {
    shutdown();
    delete static_cast<H264EncoderImpl*>(impl_);
}

bool H264Encoder::init(ID3D11Device* device, int width, int height, int fps, int bitrate_bps) {
    auto* p = static_cast<H264EncoderImpl*>(impl_);
    shutdown();

    p->device = device;
    device->GetImmediateContext(&p->context);
    p->width  = width;
    p->height = height;
    p->fps    = fps;
    p->sample_duration = 10000000LL / fps;

    // Create DXGI device manager for hardware path attempt
    HRESULT hr = MFCreateDXGIDeviceManager(&p->reset_token, &p->device_manager);
    if (SUCCEEDED(hr)) {
        hr = p->device_manager->ResetDevice(device, p->reset_token);
        if (FAILED(hr)) {
            p->device_manager->Release();
            p->device_manager = nullptr;
        }
    }

    // Create encoder (tries hardware, falls back to software)
    bool is_software = false;
    bool is_async = false;
    bool accepts_argb = false;
    IMFDXGIDeviceManager* hw_dm = nullptr;
    ID3D11Device* hw_device = nullptr;
    p->encoder = create_h264_encoder(p->device_manager, width, height, fps, bitrate_bps,
                                      is_software, is_async, accepts_argb, &hw_dm, &hw_device);
    if (!p->encoder) {
        shutdown();
        return false;
    }
    p->software_mode = is_software;
    p->is_async = is_async;

    // For async MFTs, get the event generator for event-driven processing
    if (is_async) {
        hr = p->encoder->QueryInterface(IID_PPV_ARGS(&p->event_gen));
        if (FAILED(hr)) {
            OutputDebugStringA("[H264Enc] Warning: async MFT but no event generator\n");
            p->is_async = false;
            p->event_gen = nullptr;
        }
    }

    // If hardware encoder created its own device manager, use that instead
    if (hw_dm) {
        if (p->device_manager) p->device_manager->Release();
        p->device_manager = hw_dm;
    }
    if (hw_device) {
        // Hardware encoder has its own device — we still need the capture device for CopyResource
        hw_device->Release();  // we don't own it separately; device_manager holds a ref
    }

    if (is_software) {
        // Software mode: need staging texture for CPU readback
        // Release device manager — not needed
        if (p->device_manager) { p->device_manager->Release(); p->device_manager = nullptr; }

        D3D11_TEXTURE2D_DESC td{};
        td.Width            = width;
        td.Height           = height;
        td.MipLevels        = 1;
        td.ArraySize        = 1;
        td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage            = D3D11_USAGE_STAGING;
        td.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;
        hr = device->CreateTexture2D(&td, nullptr, &p->staging);
        if (FAILED(hr)) {
            OutputDebugStringA("[H264Enc] Failed to create staging texture\n");
            shutdown(); return false;
        }

        char msg[128]; snprintf(msg, sizeof(msg), "[H264Enc] Software mode: %dx%d @ %dfps\n", width, height, fps);
        OutputDebugStringA(msg);
    } else {
        // Hardware mode
        if (accepts_argb) {
            // Encoder accepts ARGB32 directly — no color converter needed
            OutputDebugStringA("[H264Enc] Hardware mode: direct BGRA→encoder (no color converter)\n");
        } else {
            // Need color converter: BGRA → NV12
            p->color_converter = nullptr;
            HRESULT hr2 = CoCreateInstance(CLSID_VideoProcessorMFT, nullptr, CLSCTX_INPROC_SERVER,
                                           IID_PPV_ARGS(&p->color_converter));
            if (SUCCEEDED(hr2) && p->color_converter && p->device_manager) {
                IMFAttributes* attrs = nullptr;
                if (SUCCEEDED(p->color_converter->GetAttributes(&attrs)) && attrs) {
                    UINT32 d3d_aware = 0;
                    attrs->GetUINT32(MF_SA_D3D11_AWARE, &d3d_aware);
                    if (d3d_aware)
                        p->color_converter->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                            reinterpret_cast<ULONG_PTR>(p->device_manager));
                    attrs->Release();
                }
                IMFMediaType* in_mt = nullptr;
                MFCreateMediaType(&in_mt);
                in_mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                in_mt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32);
                MFSetAttributeSize(in_mt, MF_MT_FRAME_SIZE, width, height);
                MFSetAttributeRatio(in_mt, MF_MT_FRAME_RATE, fps, 1);
                in_mt->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
                p->color_converter->SetInputType(0, in_mt, 0);
                in_mt->Release();

                IMFMediaType* out_mt = nullptr;
                MFCreateMediaType(&out_mt);
                out_mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                out_mt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
                MFSetAttributeSize(out_mt, MF_MT_FRAME_SIZE, width, height);
                MFSetAttributeRatio(out_mt, MF_MT_FRAME_RATE, fps, 1);
                out_mt->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
                p->color_converter->SetOutputType(0, out_mt, 0);
                out_mt->Release();

                p->color_converter->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
                p->color_converter->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
            }
            OutputDebugStringA("[H264Enc] Hardware mode: BGRA→ColorConv→NV12→encoder\n");
        }

        char msg[128]; snprintf(msg, sizeof(msg), "[H264Enc] Hardware mode: %dx%d @ %dfps\n", width, height, fps);
        OutputDebugStringA(msg);
    }

    p->initialized = true;
    p->frame_count = 0;
    p->output_count = 0;
    p->gop_size = fps * 2;
    return true;
}

void H264Encoder::shutdown() {
    auto* p = static_cast<H264EncoderImpl*>(impl_);
    if (p->encoder) {
        p->encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        p->encoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        p->encoder->Release();
        p->encoder = nullptr;
    }
    if (p->color_converter) {
        p->color_converter->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        p->color_converter->Release();
        p->color_converter = nullptr;
    }
    if (p->event_gen) { p->event_gen->Release(); p->event_gen = nullptr; }
    if (p->staging) { p->staging->Release(); p->staging = nullptr; }
    if (p->device_manager) { p->device_manager->Release(); p->device_manager = nullptr; }
    if (p->context) { p->context->Release(); p->context = nullptr; }
    p->initialized = false;
    p->software_mode = false;
    p->is_async = false;
    p->pending_need_input = 0;
    p->sample_time = 0;
    p->frame_count = 0;
    p->output_count = 0;
}

std::vector<uint8_t> H264Encoder::encode(ID3D11Texture2D* texture, bool force_idr, bool& is_keyframe) {
    auto* p = static_cast<H264EncoderImpl*>(impl_);
    is_keyframe = false;
    if (!p->initialized || !texture) return {};

    p->frame_count++;

    // Force IDR on first frame and when requested
    if (p->frame_count == 1) force_idr = true;
    if (force_idr) {
        ICodecAPI* codec = nullptr;
        if (SUCCEEDED(p->encoder->QueryInterface(IID_PPV_ARGS(&codec)))) {
            VARIANT var;
            VariantInit(&var);
            var.vt = VT_UI4;
            var.ulVal = 1;
            codec->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &var);
            codec->Release();
        }
    }

    IMFSample* nv12_sample = nullptr;
    HRESULT hr;

    if (p->software_mode) {
        // ── Software path: GPU readback → CPU BGRA→NV12 → memory buffer ──

        // Copy texture to staging for CPU access
        p->context->CopyResource(p->staging, texture);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        hr = p->context->Map(p->staging, 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
            if (p->frame_count <= 5) {
                char msg[128]; snprintf(msg, sizeof(msg), "[H264Enc] Map staging failed hr=0x%08lX\n", hr);
                OutputDebugStringA(msg);
            }
            return {};
        }

        // Convert BGRA → NV12
        int nv12_size = p->width * p->height * 3 / 2;
        IMFMediaBuffer* nv12_buf = nullptr;
        hr = MFCreateMemoryBuffer(nv12_size, &nv12_buf);
        if (FAILED(hr)) { p->context->Unmap(p->staging, 0); return {}; }

        BYTE* nv12_data = nullptr;
        hr = nv12_buf->Lock(&nv12_data, nullptr, nullptr);
        if (SUCCEEDED(hr)) {
            bgra_to_nv12(static_cast<const uint8_t*>(mapped.pData),
                         static_cast<int>(mapped.RowPitch),
                         p->width, p->height, nv12_data);
            nv12_buf->Unlock();
            nv12_buf->SetCurrentLength(nv12_size);
        }
        p->context->Unmap(p->staging, 0);

        if (FAILED(hr)) { nv12_buf->Release(); return {}; }

        MFCreateSample(&nv12_sample);
        nv12_sample->AddBuffer(nv12_buf);
        nv12_buf->Release();

    } else if (p->color_converter) {
        // ── Hardware path with color converter: DXGI surface → CC → NV12 ──

        IMFMediaBuffer* input_buffer = nullptr;
        hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), texture, 0, FALSE, &input_buffer);
        if (FAILED(hr)) {
            if (p->frame_count <= 10) {
                char msg[128]; snprintf(msg, sizeof(msg), "[H264Enc] MFCreateDXGISurfaceBuffer failed hr=0x%08lX\n", hr);
                OutputDebugStringA(msg);
            }
            return {};
        }

        IMFSample* input_sample = nullptr;
        MFCreateSample(&input_sample);
        input_sample->AddBuffer(input_buffer);
        input_sample->SetSampleTime(p->sample_time);
        input_sample->SetSampleDuration(p->sample_duration);
        input_buffer->Release();

        hr = p->color_converter->ProcessInput(0, input_sample, 0);
        input_sample->Release();
        if (FAILED(hr)) {
            if (p->frame_count <= 10) {
                char msg[128]; snprintf(msg, sizeof(msg), "[H264Enc] ColorConv ProcessInput failed hr=0x%08lX\n", hr);
                OutputDebugStringA(msg);
            }
            return {};
        }

        MFT_OUTPUT_DATA_BUFFER cc_output{};
        DWORD cc_status = 0;
        hr = p->color_converter->ProcessOutput(0, 1, &cc_output, &cc_status);
        if (FAILED(hr) || !cc_output.pSample) {
            if (p->frame_count <= 10) {
                char msg[128]; snprintf(msg, sizeof(msg), "[H264Enc] ColorConv ProcessOutput failed hr=0x%08lX\n", hr);
                OutputDebugStringA(msg);
            }
            if (cc_output.pEvents) cc_output.pEvents->Release();
            return {};
        }
        nv12_sample = cc_output.pSample;
        if (cc_output.pEvents) cc_output.pEvents->Release();

    } else {
        // ── Hardware path direct: BGRA texture → encoder (ARGB32 input) ──

        IMFMediaBuffer* input_buffer = nullptr;
        hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), texture, 0, FALSE, &input_buffer);
        if (FAILED(hr)) {
            if (p->frame_count <= 10) {
                char msg[128]; snprintf(msg, sizeof(msg), "[H264Enc] MFCreateDXGISurfaceBuffer failed hr=0x%08lX\n", hr);
                OutputDebugStringA(msg);
            }
            return {};
        }

        MFCreateSample(&nv12_sample);  // reusing variable name — it's actually an ARGB32 sample
        nv12_sample->AddBuffer(input_buffer);
        input_buffer->Release();
    }

    // Feed sample to H.264 encoder
    nv12_sample->SetSampleTime(p->sample_time);
    nv12_sample->SetSampleDuration(p->sample_duration);

    // For async MFTs, wait for METransformNeedInput before ProcessInput
    if (p->is_async && p->event_gen) {
        bool got_need_input = (p->pending_need_input > 0);
        if (got_need_input) p->pending_need_input--;

        // Poll for events with timeout (~500ms max = 100 * 5ms)
        for (int attempts = 0; attempts < 100 && !got_need_input; attempts++) {
            IMFMediaEvent* event = nullptr;
            hr = p->event_gen->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
            if (hr == MF_E_NO_EVENTS_AVAILABLE) {
                Sleep(5);
                continue;
            }
            if (FAILED(hr)) break;
            MediaEventType met = MEUnknown;
            event->GetType(&met);
            event->Release();
            if (met == METransformNeedInput) {
                got_need_input = true;
            } else if (met == METransformHaveOutput) {
                // Drain stale output (shouldn't happen before first input)
                MFT_OUTPUT_DATA_BUFFER stale{};
                DWORD stale_status = 0;
                p->encoder->ProcessOutput(0, 1, &stale, &stale_status);
                if (stale.pSample) stale.pSample->Release();
                if (stale.pEvents) stale.pEvents->Release();
            }
        }
        if (!got_need_input) {
            if (p->frame_count <= 5) {
                OutputDebugStringA("[H264Enc] Async: never got METransformNeedInput\n");
            }
            nv12_sample->Release();
            return {};
        }
    }

    hr = p->encoder->ProcessInput(0, nv12_sample, 0);
    nv12_sample->Release();
    if (FAILED(hr)) {
        if (p->frame_count <= 5) {
            char msg[128]; snprintf(msg, sizeof(msg), "[H264Enc] ProcessInput failed hr=0x%08lX\n", hr);
            OutputDebugStringA(msg);
        }
        return {};
    }

    // For async MFTs, wait for METransformHaveOutput before ProcessOutput
    if (p->is_async && p->event_gen) {
        bool got_have_output = false;
        // Poll for output event (~500ms max)
        for (int attempts = 0; attempts < 100 && !got_have_output; attempts++) {
            IMFMediaEvent* event = nullptr;
            hr = p->event_gen->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
            if (hr == MF_E_NO_EVENTS_AVAILABLE) {
                Sleep(5);
                continue;
            }
            if (FAILED(hr)) break;
            MediaEventType met = MEUnknown;
            event->GetType(&met);
            event->Release();
            if (met == METransformHaveOutput) {
                got_have_output = true;
            } else if (met == METransformNeedInput) {
                // Buffer this for the next encode() call — don't break
                p->pending_need_input++;
            }
        }
        if (!got_have_output) {
            if (p->frame_count <= 5) {
                OutputDebugStringA("[H264Enc] Async: timed out waiting for METransformHaveOutput\n");
            }
            p->sample_time += p->sample_duration;
            return {};
        }
    }

    // Get H.264 output
    MFT_OUTPUT_STREAM_INFO stream_info{};
    p->encoder->GetOutputStreamInfo(0, &stream_info);

    MFT_OUTPUT_DATA_BUFFER enc_output{};
    enc_output.dwStreamID = 0;

    bool caller_allocates = !(stream_info.dwFlags &
        (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES));
    IMFSample* out_sample = nullptr;
    if (caller_allocates) {
        MFCreateSample(&out_sample);
        IMFMediaBuffer* out_buf = nullptr;
        DWORD buf_size = stream_info.cbSize > 0 ? stream_info.cbSize : (p->width * p->height * 2);
        MFCreateMemoryBuffer(buf_size, &out_buf);
        out_sample->AddBuffer(out_buf);
        out_buf->Release();
        enc_output.pSample = out_sample;
    }

    DWORD enc_status = 0;
    hr = p->encoder->ProcessOutput(0, 1, &enc_output, &enc_status);
    p->sample_time += p->sample_duration;

    if (FAILED(hr)) {
        if (p->frame_count <= 5) {
            char msg[128]; snprintf(msg, sizeof(msg), "[H264Enc] ProcessOutput failed hr=0x%08lX\n", hr);
            OutputDebugStringA(msg);
        }
        if (out_sample) out_sample->Release();
        if (enc_output.pEvents) enc_output.pEvents->Release();
        return {};
    }

    IMFSample* result_sample = enc_output.pSample;
    if (!result_sample) {
        if (enc_output.pEvents) enc_output.pEvents->Release();
        return {};
    }

    // Check if keyframe via MF attribute
    UINT32 clean_point = 0;
    if (SUCCEEDED(result_sample->GetUINT32(MFSampleExtension_CleanPoint, &clean_point)) && clean_point)
        is_keyframe = true;

    // Extract H.264 bytes
    IMFMediaBuffer* result_buf = nullptr;
    hr = result_sample->ConvertToContiguousBuffer(&result_buf);
    std::vector<uint8_t> output;
    if (SUCCEEDED(hr) && result_buf) {
        BYTE* data = nullptr;
        DWORD len = 0;
        hr = result_buf->Lock(&data, nullptr, &len);
        if (SUCCEEDED(hr) && data && len > 0) {
            output.assign(data, data + len);
            result_buf->Unlock();
        }
        result_buf->Release();
    }

    p->output_count++;

    // Robust keyframe detection:
    // 1) The very first output from a fresh encoder is ALWAYS an IDR (no reference exists)
    // 2) Every gop_size outputs should be a keyframe
    // 3) If we forced IDR recently and this is the next output, mark it
    if (p->output_count == 1) {
        is_keyframe = true;
    }
    if (!is_keyframe && p->gop_size > 0 && (p->output_count % p->gop_size) == 1) {
        is_keyframe = true;
    }
    if (!is_keyframe && force_idr) {
        // force_idr was set this frame — the encoder may delay output, but for
        // the software encoder (synchronous), the output should correspond to this input
        is_keyframe = true;
    }

    if (caller_allocates && out_sample) out_sample->Release();
    else if (!caller_allocates && result_sample) result_sample->Release();
    if (enc_output.pEvents) enc_output.pEvents->Release();

    // Log first few outputs and periodic status
    if (p->output_count <= 5 || (p->output_count % 100 == 0)) {
        char hex[256];
        int n = snprintf(hex, sizeof(hex), "[H264Enc] out#%d (in#%d): %zu bytes%s",
                         p->output_count, p->frame_count, output.size(),
                         is_keyframe ? " (IDR)" : "");
        if (p->output_count <= 5 && !output.empty()) {
            n += snprintf(hex + n, sizeof(hex) - n, " hex:");
            int show = output.size() < 16 ? static_cast<int>(output.size()) : 16;
            for (int i = 0; i < show && n < 230; i++)
                n += snprintf(hex + n, sizeof(hex) - n, " %02X", output[i]);
        }
        snprintf(hex + n, sizeof(hex) - n, "\n");
        OutputDebugStringA(hex);
    }

    return output;
}

void H264Encoder::set_bitrate(int bitrate_bps) {
    auto* p = static_cast<H264EncoderImpl*>(impl_);
    if (!p->encoder) return;

    ICodecAPI* codec = nullptr;
    if (SUCCEEDED(p->encoder->QueryInterface(IID_PPV_ARGS(&codec)))) {
        VARIANT var;
        VariantInit(&var);
        var.vt = VT_UI4;
        var.ulVal = static_cast<ULONG>(bitrate_bps);
        codec->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &var);
        codec->Release();
    }
}

bool H264Encoder::is_initialized() const {
    return static_cast<H264EncoderImpl*>(impl_)->initialized;
}

} // namespace lilypad
