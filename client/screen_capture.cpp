#include "screen_capture.h"

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
#include <cstdio>

namespace lilypad {

// ════════════════════════════════════════════════════════════════
//  DXGI Desktop Duplication capturer (GPU-accelerated)
// ════════════════════════════════════════════════════════════════

struct ScreenCapturerImpl {
    // DXGI state
    ID3D11Device*            device     = nullptr;
    ID3D11DeviceContext*     context    = nullptr;
    IDXGIOutputDuplication*  dup        = nullptr;
    ID3D11Texture2D*         default_copy = nullptr;  // D3D11_USAGE_DEFAULT copy for encoder
    int                      screen_w   = 0;
    int                      screen_h   = 0;
    bool                     dxgi_ok    = false;
    DXGI_FORMAT              format     = DXGI_FORMAT_B8G8R8A8_UNORM;
};

ScreenCapturer::ScreenCapturer() {
    auto* p = new ScreenCapturerImpl();
    impl_ = p;
    p->dxgi_ok = init_dxgi();
}

ScreenCapturer::~ScreenCapturer() {
    release_dxgi();
    delete static_cast<ScreenCapturerImpl*>(impl_);
}

bool ScreenCapturer::init_dxgi() {
    auto* p = static_cast<ScreenCapturerImpl*>(impl_);

    // Enumerate adapters to find the one that owns the primary display output.
    // Creating the D3D device on the correct adapter is required for both
    // Desktop Duplication and hardware H.264 encoding (NVENC/QSV).
    IDXGIFactory1* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
    if (FAILED(hr)) return false;

    IDXGIAdapter* chosen_adapter = nullptr;
    IDXGIOutput*  chosen_output  = nullptr;

    for (UINT i = 0; ; i++) {
        IDXGIAdapter* adapter = nullptr;
        if (factory->EnumAdapters(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;

        DXGI_ADAPTER_DESC desc{};
        adapter->GetDesc(&desc);
        char name[128];
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);

        IDXGIOutput* output = nullptr;
        if (SUCCEEDED(adapter->EnumOutputs(0, &output))) {
            char msg[256];
            snprintf(msg, sizeof(msg), "[ScreenCap] Adapter %u: %s (has display output)\n", i, name);
            OutputDebugStringA(msg);
            if (!chosen_adapter) {
                chosen_adapter = adapter;
                chosen_output  = output;
            } else {
                output->Release();
                adapter->Release();
            }
        } else {
            char msg[256];
            snprintf(msg, sizeof(msg), "[ScreenCap] Adapter %u: %s (no display output)\n", i, name);
            OutputDebugStringA(msg);
            adapter->Release();
        }
    }
    factory->Release();

    if (!chosen_adapter || !chosen_output) {
        if (chosen_adapter) chosen_adapter->Release();
        if (chosen_output) chosen_output->Release();
        return false;
    }

    // Create D3D device on the adapter that owns the display
    D3D_FEATURE_LEVEL level;
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
    hr = D3D11CreateDevice(
        chosen_adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels, 1, D3D11_SDK_VERSION,
        &p->device, &level, &p->context);
    chosen_adapter->Release();
    if (FAILED(hr)) {
        chosen_output->Release();
        return false;
    }

    {
        char msg[128];
        snprintf(msg, sizeof(msg), "[ScreenCap] D3D device created, feature level 0x%X\n",
                 static_cast<unsigned>(level));
        OutputDebugStringA(msg);
    }

    // Enable multithread protection — MF encoder accesses this device from internal threads
    ID3D11Multithread* mt = nullptr;
    if (SUCCEEDED(p->device->QueryInterface(__uuidof(ID3D11Multithread),
                                            reinterpret_cast<void**>(&mt)))) {
        mt->SetMultithreadProtected(TRUE);
        mt->Release();
    }

    IDXGIOutput1* output1 = nullptr;
    hr = chosen_output->QueryInterface(__uuidof(IDXGIOutput1),
                                reinterpret_cast<void**>(&output1));
    chosen_output->Release();
    if (FAILED(hr)) return false;

    hr = output1->DuplicateOutput(p->device, &p->dup);
    output1->Release();
    if (FAILED(hr)) return false;

    DXGI_OUTDUPL_DESC dup_desc;
    p->dup->GetDesc(&dup_desc);
    p->screen_w = static_cast<int>(dup_desc.ModeDesc.Width);
    p->screen_h = static_cast<int>(dup_desc.ModeDesc.Height);
    p->format   = dup_desc.ModeDesc.Format;

    // Create DEFAULT usage texture for GPU→GPU copy (encoder input)
    D3D11_TEXTURE2D_DESC td{};
    td.Width            = p->screen_w;
    td.Height           = p->screen_h;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = p->format;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = 0;  // no bind flags needed — MF uses it via DXGI surface buffer

    hr = p->device->CreateTexture2D(&td, nullptr, &p->default_copy);
    if (FAILED(hr)) {
        p->dup->Release(); p->dup = nullptr;
        return false;
    }

    return true;
}

void ScreenCapturer::release_dxgi() {
    auto* p = static_cast<ScreenCapturerImpl*>(impl_);
    if (p->default_copy) { p->default_copy->Release(); p->default_copy = nullptr; }
    if (p->dup)          { p->dup->Release();          p->dup          = nullptr; }
    if (p->context)      { p->context->Release();      p->context      = nullptr; }
    if (p->device)       { p->device->Release();       p->device       = nullptr; }
    p->dxgi_ok = false;
}

ID3D11Texture2D* ScreenCapturer::capture_texture(int& out_width, int& out_height) {
    auto* p = static_cast<ScreenCapturerImpl*>(impl_);
    if (!p->dxgi_ok) return nullptr;

    IDXGIResource* resource = nullptr;
    DXGI_OUTDUPL_FRAME_INFO frame_info{};
    HRESULT hr = p->dup->AcquireNextFrame(16, &frame_info, &resource);

    if (hr == DXGI_ERROR_ACCESS_LOST) {
        release_dxgi();
        p->dxgi_ok = init_dxgi();
        if (!p->dxgi_ok) return nullptr;
        hr = p->dup->AcquireNextFrame(16, &frame_info, &resource);
    }

    if (FAILED(hr)) return nullptr;

    ID3D11Texture2D* frame_tex = nullptr;
    hr = resource->QueryInterface(__uuidof(ID3D11Texture2D),
                                  reinterpret_cast<void**>(&frame_tex));
    resource->Release();
    if (FAILED(hr)) { p->dup->ReleaseFrame(); return nullptr; }

    // GPU→GPU copy to our DEFAULT texture (no CPU readback)
    p->context->CopyResource(p->default_copy, frame_tex);
    frame_tex->Release();
    p->dup->ReleaseFrame();

    out_width  = p->screen_w;
    out_height = p->screen_h;
    return p->default_copy;
}

ID3D11Device* ScreenCapturer::get_device() const {
    return static_cast<ScreenCapturerImpl*>(impl_)->device;
}

ID3D11DeviceContext* ScreenCapturer::get_context() const {
    return static_cast<ScreenCapturerImpl*>(impl_)->context;
}

int ScreenCapturer::screen_width() const {
    return static_cast<ScreenCapturerImpl*>(impl_)->screen_w;
}

int ScreenCapturer::screen_height() const {
    return static_cast<ScreenCapturerImpl*>(impl_)->screen_h;
}

} // namespace lilypad
