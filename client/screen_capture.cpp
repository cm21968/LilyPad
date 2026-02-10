#include "screen_capture.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <turbojpeg.h>

#include <algorithm>

namespace lilypad {

// ════════════════════════════════════════════════════════════════
//  DXGI Desktop Duplication capturer (GPU-accelerated)
// ════════════════════════════════════════════════════════════════

struct ScreenCapturerImpl {
    // DXGI state
    ID3D11Device*            device   = nullptr;
    ID3D11DeviceContext*     context  = nullptr;
    IDXGIOutputDuplication*  dup      = nullptr;
    ID3D11Texture2D*         staging  = nullptr;
    int                      screen_w = 0;
    int                      screen_h = 0;
    bool                     dxgi_ok  = false;

    // Persistent turbojpeg compressor (reused across frames)
    tjhandle                 tj       = nullptr;
};

ScreenCapturer::ScreenCapturer() {
    auto* p = new ScreenCapturerImpl();
    impl_ = p;
    p->tj = tjInitCompress();
    p->dxgi_ok = init_dxgi();
}

ScreenCapturer::~ScreenCapturer() {
    release_dxgi();
    auto* p = static_cast<ScreenCapturerImpl*>(impl_);
    if (p->tj) tjDestroy(p->tj);
    delete p;
}

bool ScreenCapturer::init_dxgi() {
    auto* p = static_cast<ScreenCapturerImpl*>(impl_);

    D3D_FEATURE_LEVEL level;
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        levels, 1, D3D11_SDK_VERSION,
        &p->device, &level, &p->context);
    if (FAILED(hr)) return false;

    IDXGIDevice* dxgi_dev = nullptr;
    hr = p->device->QueryInterface(__uuidof(IDXGIDevice),
                                   reinterpret_cast<void**>(&dxgi_dev));
    if (FAILED(hr)) return false;

    IDXGIAdapter* adapter = nullptr;
    hr = dxgi_dev->GetAdapter(&adapter);
    dxgi_dev->Release();
    if (FAILED(hr)) return false;

    IDXGIOutput* output = nullptr;
    hr = adapter->EnumOutputs(0, &output);
    adapter->Release();
    if (FAILED(hr)) return false;

    IDXGIOutput1* output1 = nullptr;
    hr = output->QueryInterface(__uuidof(IDXGIOutput1),
                                reinterpret_cast<void**>(&output1));
    output->Release();
    if (FAILED(hr)) return false;

    hr = output1->DuplicateOutput(p->device, &p->dup);
    output1->Release();
    if (FAILED(hr)) return false;

    DXGI_OUTDUPL_DESC dup_desc;
    p->dup->GetDesc(&dup_desc);
    p->screen_w = static_cast<int>(dup_desc.ModeDesc.Width);
    p->screen_h = static_cast<int>(dup_desc.ModeDesc.Height);

    // Create staging texture for CPU readback — use the actual output format
    D3D11_TEXTURE2D_DESC td{};
    td.Width            = p->screen_w;
    td.Height           = p->screen_h;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = dup_desc.ModeDesc.Format;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_STAGING;
    td.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;

    hr = p->device->CreateTexture2D(&td, nullptr, &p->staging);
    if (FAILED(hr)) {
        p->dup->Release(); p->dup = nullptr;
        return false;
    }

    return true;
}

void ScreenCapturer::release_dxgi() {
    auto* p = static_cast<ScreenCapturerImpl*>(impl_);
    if (p->staging) { p->staging->Release(); p->staging = nullptr; }
    if (p->dup)     { p->dup->Release();     p->dup     = nullptr; }
    if (p->context) { p->context->Release(); p->context = nullptr; }
    if (p->device)  { p->device->Release();  p->device  = nullptr; }
    p->dxgi_ok = false;
}

std::vector<uint8_t> ScreenCapturer::capture_jpeg(
        int quality, int target_w, int target_h,
        int& out_width, int& out_height) {
    auto* p = static_cast<ScreenCapturerImpl*>(impl_);

    // Fall back to GDI if DXGI not available
    if (!p->dxgi_ok) {
        return capture_screen_jpeg(quality, target_w, target_h,
                                   out_width, out_height);
    }

    // Acquire next frame from DXGI Desktop Duplication
    IDXGIResource* resource = nullptr;
    DXGI_OUTDUPL_FRAME_INFO frame_info{};
    HRESULT hr = p->dup->AcquireNextFrame(16, &frame_info, &resource);

    if (hr == DXGI_ERROR_ACCESS_LOST) {
        // Desktop switch, resolution change — recreate
        release_dxgi();
        p->dxgi_ok = init_dxgi();
        if (!p->dxgi_ok)
            return capture_screen_jpeg(quality, target_w, target_h,
                                       out_width, out_height);
        hr = p->dup->AcquireNextFrame(16, &frame_info, &resource);
    }

    if (FAILED(hr)) return {};  // timeout — no new frame available

    ID3D11Texture2D* frame_tex = nullptr;
    hr = resource->QueryInterface(__uuidof(ID3D11Texture2D),
                                  reinterpret_cast<void**>(&frame_tex));
    resource->Release();
    if (FAILED(hr)) { p->dup->ReleaseFrame(); return {}; }

    // Copy GPU frame to staging texture for CPU access
    p->context->CopyResource(p->staging, frame_tex);
    frame_tex->Release();
    p->dup->ReleaseFrame();

    // Map staging texture to CPU memory
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = p->context->Map(p->staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return {};

    // Determine output dimensions
    int src_w = p->screen_w;
    int src_h = p->screen_h;
    int cap_w = src_w;
    int cap_h = src_h;

    bool need_scale = (cap_w > target_w || cap_h > target_h);
    if (need_scale) {
        float scale = (std::min)(static_cast<float>(target_w) / src_w,
                                 static_cast<float>(target_h) / src_h);
        cap_w = static_cast<int>(src_w * scale);
        cap_h = static_cast<int>(src_h * scale);
    }
    cap_w &= ~1;
    cap_h &= ~1;
    out_width  = cap_w;
    out_height = cap_h;

    // Prepare pixel data for JPEG encoding
    const uint8_t* encode_src;
    int encode_pitch;
    std::vector<uint8_t> scaled_pixels;

    if (!need_scale) {
        // Encode directly from mapped GPU memory — zero copy
        encode_src   = static_cast<const uint8_t*>(mapped.pData);
        encode_pitch = static_cast<int>(mapped.RowPitch);
    } else {
        // Nearest-neighbor downscale (very fast, JPEG smooths any artifacts)
        scaled_pixels.resize(cap_w * 4 * cap_h);
        encode_pitch = cap_w * 4;
        float x_ratio = static_cast<float>(src_w) / cap_w;
        float y_ratio = static_cast<float>(src_h) / cap_h;
        const uint8_t* src_data = static_cast<const uint8_t*>(mapped.pData);
        int src_pitch = static_cast<int>(mapped.RowPitch);

        for (int y = 0; y < cap_h; ++y) {
            int sy = static_cast<int>(y * y_ratio);
            const uint8_t* src_row = src_data + sy * src_pitch;
            uint8_t* dst_row = scaled_pixels.data() + y * encode_pitch;
            for (int x = 0; x < cap_w; ++x) {
                int sx = static_cast<int>(x * x_ratio);
                const uint8_t* sp = src_row + sx * 4;
                uint8_t* dp = dst_row + x * 4;
                dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = sp[3];
            }
        }
        encode_src = scaled_pixels.data();
    }

    // JPEG encode with persistent handle (no per-frame alloc)
    unsigned char* jpeg_buf  = nullptr;
    unsigned long  jpeg_size = 0;

    int rc = tjCompress2(p->tj, encode_src, cap_w, encode_pitch, cap_h,
                         TJPF_BGRX,        // BGRA pixel format (X = ignored alpha)
                         &jpeg_buf, &jpeg_size,
                         TJSAMP_420,       // 4:2:0 chroma subsampling
                         quality,
                         TJFLAG_FASTDCT);  // fast DCT

    p->context->Unmap(p->staging, 0);

    std::vector<uint8_t> result;
    if (rc == 0 && jpeg_buf) {
        result.assign(jpeg_buf, jpeg_buf + jpeg_size);
    }
    tjFree(jpeg_buf);
    return result;
}

// ════════════════════════════════════════════════════════════════
//  Legacy GDI capture (fallback if DXGI unavailable)
// ════════════════════════════════════════════════════════════════

std::vector<uint8_t> capture_screen_jpeg(int quality, int target_w, int target_h,
                                          int& out_width, int& out_height) {
    int screen_w = GetSystemMetrics(SM_CXSCREEN);
    int screen_h = GetSystemMetrics(SM_CYSCREEN);

    int cap_w = screen_w;
    int cap_h = screen_h;
    if (cap_w > target_w || cap_h > target_h) {
        float scale = (std::min)(static_cast<float>(target_w) / screen_w,
                                 static_cast<float>(target_h) / screen_h);
        cap_w = static_cast<int>(screen_w * scale);
        cap_h = static_cast<int>(screen_h * scale);
    }
    cap_w &= ~1;
    cap_h &= ~1;

    out_width  = cap_w;
    out_height = cap_h;

    HDC hdc_screen = GetDC(nullptr);
    HDC hdc_mem    = CreateCompatibleDC(hdc_screen);
    HBITMAP hbm    = CreateCompatibleBitmap(hdc_screen, cap_w, cap_h);
    HGDIOBJ old_bm = SelectObject(hdc_mem, hbm);

    SetStretchBltMode(hdc_mem, HALFTONE);
    StretchBlt(hdc_mem, 0, 0, cap_w, cap_h,
               hdc_screen, 0, 0, screen_w, screen_h, SRCCOPY);

    BITMAPINFOHEADER bi{};
    bi.biSize        = sizeof(bi);
    bi.biWidth       = cap_w;
    bi.biHeight      = -cap_h;
    bi.biPlanes      = 1;
    bi.biBitCount    = 32;
    bi.biCompression = BI_RGB;

    int pitch = cap_w * 4;
    std::vector<uint8_t> pixels(pitch * cap_h);

    GetDIBits(hdc_mem, hbm, 0, cap_h, pixels.data(),
              reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);

    SelectObject(hdc_mem, old_bm);
    DeleteObject(hbm);
    DeleteDC(hdc_mem);
    ReleaseDC(nullptr, hdc_screen);

    tjhandle tj = tjInitCompress();
    if (!tj) return {};

    unsigned char* jpeg_buf  = nullptr;
    unsigned long  jpeg_size = 0;

    int rc = tjCompress2(tj, pixels.data(), cap_w, pitch, cap_h,
                         TJPF_BGRX,
                         &jpeg_buf, &jpeg_size,
                         TJSAMP_420,
                         quality,
                         TJFLAG_FASTDCT);

    std::vector<uint8_t> result;
    if (rc == 0 && jpeg_buf) {
        result.assign(jpeg_buf, jpeg_buf + jpeg_size);
    }

    tjFree(jpeg_buf);
    tjDestroy(tj);
    return result;
}

// ════════════════════════════════════════════════════════════════
//  JPEG decoder (persistent handle via thread-local)
// ════════════════════════════════════════════════════════════════

std::vector<uint8_t> decode_jpeg_to_rgba(const uint8_t* jpeg_data, size_t jpeg_len,
                                          int& out_width, int& out_height) {
    // Reuse decompressor across calls on the same thread
    thread_local tjhandle tj = tjInitDecompress();
    if (!tj) return {};

    int w = 0, h = 0, subsamp = 0, colorspace = 0;
    if (tjDecompressHeader3(tj, jpeg_data, static_cast<unsigned long>(jpeg_len),
                             &w, &h, &subsamp, &colorspace) != 0) {
        return {};
    }

    out_width  = w;
    out_height = h;
    int pitch = w * 4;

    std::vector<uint8_t> rgba(pitch * h);
    int rc = tjDecompress2(tj, jpeg_data, static_cast<unsigned long>(jpeg_len),
                            rgba.data(), w, pitch, h,
                            TJPF_RGBA,
                            TJFLAG_FASTDCT);

    if (rc != 0) return {};
    return rgba;
}

} // namespace lilypad
