#include "h264_decoder.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <initguid.h>
#include <wmcodecdsp.h>

#include <cstdio>
#include <codecapi.h>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

namespace lilypad {

// ════════════════════════════════════════════════════════════════
//  H.264 Decoder Implementation
//
//  Pipeline: H.264 → decoder MFT (NV12) → VideoProcessorMFT (NV12→RGBA) → SRV
//  Uses IMFDXGIDeviceManager wrapping the main rendering device.
// ════════════════════════════════════════════════════════════════

struct H264DecoderImpl {
    IMFTransform*            decoder         = nullptr;  // H.264 decoder MFT
    IMFTransform*            color_converter = nullptr;  // Video Processor MFT (NV12→RGBA)
    IMFDXGIDeviceManager*    device_manager  = nullptr;
    UINT                     reset_token     = 0;
    ID3D11Device*            device          = nullptr;  // not owned
    ID3D11DeviceContext*     context         = nullptr;  // not owned
    ID3D11Texture2D*         output_texture  = nullptr;  // RGBA output
    ID3D11ShaderResourceView* output_srv     = nullptr;
    int                      width           = 0;
    int                      height          = 0;
    bool                     initialized     = false;
    bool                     got_keyframe    = false;
    LONGLONG                 sample_time     = 0;
};

// Helper: create hardware H.264 decoder MFT
static IMFTransform* create_h264_decoder(IMFDXGIDeviceManager* dm) {
    IMFActivate** activates = nullptr;
    UINT32 count = 0;

    MFT_REGISTER_TYPE_INFO input_type = { MFMediaType_Video, MFVideoFormat_H264 };

    HRESULT hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_DECODER,
        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
        &input_type,
        nullptr,
        &activates,
        &count);

    if (FAILED(hr) || count == 0) {
        // Fall back to software decoder
        hr = MFTEnumEx(
            MFT_CATEGORY_VIDEO_DECODER,
            MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
            &input_type,
            nullptr,
            &activates,
            &count);
        if (FAILED(hr) || count == 0) return nullptr;
    }

    IMFTransform* decoder = nullptr;
    hr = activates[0]->ActivateObject(IID_PPV_ARGS(&decoder));

    for (UINT32 i = 0; i < count; i++) activates[i]->Release();
    CoTaskMemFree(activates);

    if (FAILED(hr)) return nullptr;

    // Enable D3D11 acceleration
    if (dm) {
        IMFAttributes* attrs = nullptr;
        hr = decoder->GetAttributes(&attrs);
        if (SUCCEEDED(hr) && attrs) {
            UINT32 d3d_aware = 0;
            attrs->GetUINT32(MF_SA_D3D11_AWARE, &d3d_aware);
            if (d3d_aware) {
                decoder->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(dm));
            }
            // Enable low latency
            attrs->SetUINT32(CODECAPI_AVLowLatencyMode, TRUE);
            attrs->Release();
        }
    }

    // Set input type: H.264
    IMFMediaType* in_type = nullptr;
    MFCreateMediaType(&in_type);
    in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    hr = decoder->SetInputType(0, in_type, 0);
    in_type->Release();
    if (FAILED(hr)) { decoder->Release(); return nullptr; }

    // Let the decoder choose output type (typically NV12)
    // We'll negotiate output after first decode
    for (DWORD i = 0; ; i++) {
        IMFMediaType* avail = nullptr;
        hr = decoder->GetOutputAvailableType(0, i, &avail);
        if (FAILED(hr)) break;
        GUID subtype;
        avail->GetGUID(MF_MT_SUBTYPE, &subtype);
        if (subtype == MFVideoFormat_NV12) {
            decoder->SetOutputType(0, avail, 0);
            avail->Release();
            break;
        }
        avail->Release();
    }

    decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    return decoder;
}

// Helper: configure the color converter for NV12→RGBA at given resolution
static bool setup_color_converter(IMFTransform* vp, IMFDXGIDeviceManager* dm, int w, int h) {
    if (!vp) return false;

    // Enable D3D11 if available
    if (dm) {
        IMFAttributes* attrs = nullptr;
        if (SUCCEEDED(vp->GetAttributes(&attrs)) && attrs) {
            UINT32 d3d_aware = 0;
            attrs->GetUINT32(MF_SA_D3D11_AWARE, &d3d_aware);
            if (d3d_aware) {
                vp->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(dm));
            }
            attrs->Release();
        }
    }

    // Input: NV12
    IMFMediaType* in_type = nullptr;
    MFCreateMediaType(&in_type);
    in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(in_type, MF_MT_FRAME_SIZE, w, h);
    in_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    HRESULT hr = vp->SetInputType(0, in_type, 0);
    in_type->Release();
    if (FAILED(hr)) return false;

    // Output: BGRA with alpha (MFVideoFormat_ARGB32 = D3DFMT_A8R8G8B8 = DXGI B8G8R8A8_UNORM)
    // Using ARGB32 instead of RGB32 ensures alpha=0xFF (opaque), matching our output texture.
    IMFMediaType* out_type = nullptr;
    MFCreateMediaType(&out_type);
    out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32);
    MFSetAttributeSize(out_type, MF_MT_FRAME_SIZE, w, h);
    out_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    hr = vp->SetOutputType(0, out_type, 0);
    if (FAILED(hr)) {
        // Fallback to RGB32 if ARGB32 not supported
        out_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        hr = vp->SetOutputType(0, out_type, 0);
    }
    out_type->Release();
    if (FAILED(hr)) return false;

    vp->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    vp->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

// Helper: create or recreate the RGBA output texture and SRV
static bool create_output_texture(H264DecoderImpl* p, int w, int h) {
    if (p->output_srv) { p->output_srv->Release(); p->output_srv = nullptr; }
    if (p->output_texture) { p->output_texture->Release(); p->output_texture = nullptr; }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width            = w;
    desc.Height           = h;
    desc.MipLevels        = 1;
    desc.ArraySize        = 1;
    desc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage            = D3D11_USAGE_DEFAULT;
    desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = p->device->CreateTexture2D(&desc, nullptr, &p->output_texture);
    if (FAILED(hr)) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format                    = desc.Format;
    srv_desc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels       = 1;
    hr = p->device->CreateShaderResourceView(p->output_texture, &srv_desc, &p->output_srv);
    if (FAILED(hr)) {
        p->output_texture->Release();
        p->output_texture = nullptr;
        return false;
    }

    p->width  = w;
    p->height = h;
    return true;
}

H264Decoder::H264Decoder() {
    impl_ = new H264DecoderImpl();
}

H264Decoder::~H264Decoder() {
    shutdown();
    delete static_cast<H264DecoderImpl*>(impl_);
}

bool H264Decoder::init(ID3D11Device* device) {
    auto* p = static_cast<H264DecoderImpl*>(impl_);
    shutdown();

    p->device = device;
    device->GetImmediateContext(&p->context);

    // Enable multithread protection on the device (we decode on a separate thread)
    ID3D11Multithread* mt = nullptr;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&mt)))) {
        mt->SetMultithreadProtected(TRUE);
        mt->Release();
    }

    // Create DXGI device manager
    HRESULT hr = MFCreateDXGIDeviceManager(&p->reset_token, &p->device_manager);
    if (FAILED(hr)) { shutdown(); return false; }

    hr = p->device_manager->ResetDevice(device, p->reset_token);
    if (FAILED(hr)) { shutdown(); return false; }

    // Create H.264 decoder
    p->decoder = create_h264_decoder(p->device_manager);
    if (!p->decoder) { shutdown(); return false; }

    // Color converter will be configured once we know the output resolution
    hr = CoCreateInstance(CLSID_VideoProcessorMFT, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&p->color_converter));
    if (FAILED(hr)) { shutdown(); return false; }

    p->initialized = true;
    p->got_keyframe = false;
    return true;
}

void H264Decoder::shutdown() {
    auto* p = static_cast<H264DecoderImpl*>(impl_);

    if (p->decoder) {
        p->decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        p->decoder->Release();
        p->decoder = nullptr;
    }
    if (p->color_converter) {
        p->color_converter->Release();
        p->color_converter = nullptr;
    }
    if (p->output_srv) { p->output_srv->Release(); p->output_srv = nullptr; }
    if (p->output_texture) { p->output_texture->Release(); p->output_texture = nullptr; }
    if (p->device_manager) { p->device_manager->Release(); p->device_manager = nullptr; }
    if (p->context) { p->context->Release(); p->context = nullptr; }

    p->width = 0;
    p->height = 0;
    p->initialized = false;
    p->got_keyframe = false;
    p->sample_time = 0;
}

// Helper: call ProcessOutput with proper sample allocation if needed
static HRESULT process_output_with_alloc(IMFTransform* mft, MFT_OUTPUT_DATA_BUFFER& output) {
    memset(&output, 0, sizeof(output));
    output.dwStreamID = 0;

    MFT_OUTPUT_STREAM_INFO stream_info{};
    mft->GetOutputStreamInfo(0, &stream_info);

    bool caller_alloc = !(stream_info.dwFlags &
        (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES));

    IMFSample* alloc_sample = nullptr;
    if (caller_alloc) {
        MFCreateSample(&alloc_sample);
        IMFMediaBuffer* buf = nullptr;
        DWORD size = stream_info.cbSize > 0 ? stream_info.cbSize : (1920 * 1080 * 4);
        MFCreateMemoryBuffer(size, &buf);
        alloc_sample->AddBuffer(buf);
        buf->Release();
        output.pSample = alloc_sample;
    }

    DWORD status = 0;
    HRESULT hr = mft->ProcessOutput(0, 1, &output, &status);

    if (FAILED(hr) && caller_alloc && alloc_sample) {
        alloc_sample->Release();
        output.pSample = nullptr;
    }
    return hr;
}

bool H264Decoder::decode(const uint8_t* data, size_t len, bool is_keyframe) {
    auto* p = static_cast<H264DecoderImpl*>(impl_);
    if (!p->initialized || !data || len == 0) return false;

    static int dec_frame_count = 0;
    dec_frame_count++;

    // Discard P-frames until we get a keyframe
    if (!p->got_keyframe) {
        if (!is_keyframe) return false;
        p->got_keyframe = true;
        OutputDebugStringA("[H264Dec] Got first keyframe\n");
    }

    // Create input sample from H.264 data
    IMFMediaBuffer* input_buffer = nullptr;
    HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(len), &input_buffer);
    if (FAILED(hr)) return false;

    BYTE* buf_data = nullptr;
    hr = input_buffer->Lock(&buf_data, nullptr, nullptr);
    if (SUCCEEDED(hr)) {
        memcpy(buf_data, data, len);
        input_buffer->Unlock();
        input_buffer->SetCurrentLength(static_cast<DWORD>(len));
    }

    IMFSample* input_sample = nullptr;
    MFCreateSample(&input_sample);
    input_sample->AddBuffer(input_buffer);
    input_sample->SetSampleTime(p->sample_time);
    input_sample->SetSampleDuration(333333);  // ~30fps in 100ns units
    p->sample_time += 333333;
    input_buffer->Release();

    // Feed to decoder
    hr = p->decoder->ProcessInput(0, input_sample, 0);
    input_sample->Release();
    if (FAILED(hr)) {
        if (dec_frame_count <= 5) {
            char msg[128]; snprintf(msg, sizeof(msg), "[H264Dec] ProcessInput failed hr=0x%08lX\n", hr);
            OutputDebugStringA(msg);
        }
        return false;
    }

    // Try to get decoded output
    MFT_OUTPUT_DATA_BUFFER dec_output{};
    hr = process_output_with_alloc(p->decoder, dec_output);

    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        OutputDebugStringA("[H264Dec] Stream change — re-negotiating output type\n");
        // Resolution changed — re-negotiate output type
        for (DWORD i = 0; ; i++) {
            IMFMediaType* avail = nullptr;
            HRESULT hr2 = p->decoder->GetOutputAvailableType(0, i, &avail);
            if (FAILED(hr2)) break;
            GUID subtype;
            avail->GetGUID(MF_MT_SUBTYPE, &subtype);
            if (subtype == MFVideoFormat_NV12) {
                p->decoder->SetOutputType(0, avail, 0);

                // Get new dimensions
                UINT32 new_w = 0, new_h = 0;
                MFGetAttributeSize(avail, MF_MT_FRAME_SIZE, &new_w, &new_h);
                avail->Release();

                if (new_w > 0 && new_h > 0) {
                    char msg[128]; snprintf(msg, sizeof(msg),
                        "[H264Dec] New resolution: %ux%u\n", new_w, new_h);
                    OutputDebugStringA(msg);
                    setup_color_converter(p->color_converter, p->device_manager,
                                          static_cast<int>(new_w), static_cast<int>(new_h));
                    create_output_texture(p, static_cast<int>(new_w), static_cast<int>(new_h));
                }
                break;
            }
            avail->Release();
        }

        // Retry ProcessOutput
        hr = process_output_with_alloc(p->decoder, dec_output);
    }

    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        if (dec_frame_count <= 5) OutputDebugStringA("[H264Dec] Decoder needs more input\n");
        if (dec_output.pEvents) dec_output.pEvents->Release();
        return false;
    }

    if (FAILED(hr) || !dec_output.pSample) {
        if (dec_frame_count <= 5) {
            char msg[128]; snprintf(msg, sizeof(msg), "[H264Dec] Decoder ProcessOutput failed hr=0x%08lX\n", hr);
            OutputDebugStringA(msg);
        }
        if (dec_output.pEvents) dec_output.pEvents->Release();
        return false;
    }

    IMFSample* nv12_sample = dec_output.pSample;

    // If we don't have dimensions yet (first frame), configure color converter now
    if (p->width == 0 || p->height == 0) {
        IMFMediaType* cur_type = nullptr;
        if (SUCCEEDED(p->decoder->GetOutputCurrentType(0, &cur_type))) {
            UINT32 w = 0, h = 0;
            MFGetAttributeSize(cur_type, MF_MT_FRAME_SIZE, &w, &h);
            cur_type->Release();
            if (w > 0 && h > 0) {
                char msg[128]; snprintf(msg, sizeof(msg),
                    "[H264Dec] First frame resolution: %ux%u\n", w, h);
                OutputDebugStringA(msg);
                setup_color_converter(p->color_converter, p->device_manager,
                                      static_cast<int>(w), static_cast<int>(h));
                create_output_texture(p, static_cast<int>(w), static_cast<int>(h));
            }
        }
    }

    if (p->width == 0 || p->height == 0) {
        nv12_sample->Release();
        if (dec_output.pEvents) dec_output.pEvents->Release();
        return false;
    }

    // Feed NV12 to color converter
    hr = p->color_converter->ProcessInput(0, nv12_sample, 0);
    nv12_sample->Release();
    if (dec_output.pEvents) dec_output.pEvents->Release();
    if (FAILED(hr)) {
        if (dec_frame_count <= 5) {
            char msg[128]; snprintf(msg, sizeof(msg), "[H264Dec] Color converter ProcessInput failed hr=0x%08lX\n", hr);
            OutputDebugStringA(msg);
        }
        return false;
    }

    // Get RGBA output
    MFT_OUTPUT_DATA_BUFFER cc_output{};
    hr = process_output_with_alloc(p->color_converter, cc_output);
    if (FAILED(hr) || !cc_output.pSample) {
        if (dec_frame_count <= 5) {
            char msg[128]; snprintf(msg, sizeof(msg), "[H264Dec] Color converter ProcessOutput failed hr=0x%08lX\n", hr);
            OutputDebugStringA(msg);
        }
        if (cc_output.pEvents) cc_output.pEvents->Release();
        return false;
    }

    // Copy the RGBA output to our output texture
    IMFMediaBuffer* out_buf = nullptr;
    hr = cc_output.pSample->ConvertToContiguousBuffer(&out_buf);
    if (SUCCEEDED(hr) && out_buf) {
        // Try to get a DXGI surface buffer for GPU→GPU copy
        IMFDXGIBuffer* dxgi_buf = nullptr;
        hr = out_buf->QueryInterface(IID_PPV_ARGS(&dxgi_buf));
        if (SUCCEEDED(hr) && dxgi_buf) {
            ID3D11Texture2D* src_tex = nullptr;
            UINT subresource = 0;
            hr = dxgi_buf->GetResource(IID_PPV_ARGS(&src_tex));
            dxgi_buf->GetSubresourceIndex(&subresource);
            if (SUCCEEDED(hr) && src_tex && p->output_texture) {
                p->context->CopySubresourceRegion(p->output_texture, 0, 0, 0, 0,
                                                   src_tex, subresource, nullptr);
                src_tex->Release();
                dxgi_buf->Release();
                out_buf->Release();
                cc_output.pSample->Release();
                if (cc_output.pEvents) cc_output.pEvents->Release();
                if (dec_frame_count <= 3) OutputDebugStringA("[H264Dec] Decoded frame (GPU path)\n");
                return true;
            }
            if (src_tex) src_tex->Release();
            dxgi_buf->Release();
        }

        // Fallback: CPU copy
        BYTE* pixels = nullptr;
        DWORD max_len = 0, cur_len = 0;
        hr = out_buf->Lock(&pixels, &max_len, &cur_len);
        if (SUCCEEDED(hr) && pixels && p->output_texture) {
            D3D11_TEXTURE2D_DESC desc{};
            p->output_texture->GetDesc(&desc);

            ID3D11Texture2D* staging = nullptr;
            D3D11_TEXTURE2D_DESC staging_desc = desc;
            staging_desc.Usage = D3D11_USAGE_STAGING;
            staging_desc.BindFlags = 0;
            staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            hr = p->device->CreateTexture2D(&staging_desc, nullptr, &staging);
            if (SUCCEEDED(hr) && staging) {
                D3D11_MAPPED_SUBRESOURCE mapped{};
                hr = p->context->Map(staging, 0, D3D11_MAP_WRITE, 0, &mapped);
                if (SUCCEEDED(hr)) {
                    int row_bytes = p->width * 4;
                    for (int y = 0; y < p->height; ++y) {
                        memcpy(static_cast<BYTE*>(mapped.pData) + y * mapped.RowPitch,
                               pixels + y * row_bytes, row_bytes);
                    }
                    p->context->Unmap(staging, 0);
                    p->context->CopyResource(p->output_texture, staging);
                }
                staging->Release();
            }
            out_buf->Unlock();
        }
        out_buf->Release();
    }

    cc_output.pSample->Release();
    if (cc_output.pEvents) cc_output.pEvents->Release();
    if (dec_frame_count <= 3) OutputDebugStringA("[H264Dec] Decoded frame (CPU fallback)\n");
    return true;
}

ID3D11ShaderResourceView* H264Decoder::get_output_srv() const {
    return static_cast<H264DecoderImpl*>(impl_)->output_srv;
}

int H264Decoder::width() const {
    return static_cast<H264DecoderImpl*>(impl_)->width;
}

int H264Decoder::height() const {
    return static_cast<H264DecoderImpl*>(impl_)->height;
}

void H264Decoder::flush() {
    auto* p = static_cast<H264DecoderImpl*>(impl_);
    if (p->decoder) {
        p->decoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        p->decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        p->decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    }
    if (p->color_converter) {
        p->color_converter->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        p->color_converter->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        p->color_converter->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    }
    p->got_keyframe = false;
    p->sample_time = 0;
}

bool H264Decoder::is_initialized() const {
    return static_cast<H264DecoderImpl*>(impl_)->initialized;
}

} // namespace lilypad
