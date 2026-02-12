#include "d3d_helpers.h"

// ── D3D11 globals ──
ID3D11Device*           g_d3d_device    = nullptr;
ID3D11DeviceContext*    g_d3d_context   = nullptr;
IDXGISwapChain*         g_swap_chain    = nullptr;
ID3D11RenderTargetView* g_rtv           = nullptr;

// ── Custom title bar ──
HWND  g_hwnd = nullptr;
bool  g_cursor_on_titlebar = false;
bool  g_options_menu_open = false;
ImVec2 g_gear_btn_pos = ImVec2(0, 0);

bool CreateD3DDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount        = 2;
    sd.BufferDesc.Width   = 0;
    sd.BufferDesc.Height  = 0;
    sd.BufferDesc.Format  = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator   = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags              = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow       = hwnd;
    sd.SampleDesc.Count   = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed           = TRUE;
    sd.SwapEffect         = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL level;
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        levels, 1, D3D11_SDK_VERSION, &sd,
        &g_swap_chain, &g_d3d_device, &level, &g_d3d_context);

    if (FAILED(hr)) return false;

    ID3D11Texture2D* back_buffer = nullptr;
    g_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    if (!back_buffer) return false;
    g_d3d_device->CreateRenderTargetView(back_buffer, nullptr, &g_rtv);
    back_buffer->Release();
    return true;
}

void CleanupD3D() {
    if (g_rtv)        { g_rtv->Release();        g_rtv = nullptr; }
    if (g_swap_chain) { g_swap_chain->Release();  g_swap_chain = nullptr; }
    if (g_d3d_context){ g_d3d_context->Release(); g_d3d_context = nullptr; }
    if (g_d3d_device) { g_d3d_device->Release();  g_d3d_device = nullptr; }
}

void ResizeD3D(UINT width, UINT height) {
    if (!g_d3d_device || width == 0 || height == 0) return;
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    g_swap_chain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    ID3D11Texture2D* back_buffer = nullptr;
    g_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    if (!back_buffer) return;
    g_d3d_device->CreateRenderTargetView(back_buffer, nullptr, &g_rtv);
    back_buffer->Release();
}
