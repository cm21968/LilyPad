#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <d3d11.h>
#include <dxgi.h>
#include <windows.h>
#include <imgui.h>

// ── D3D11 globals (defined in d3d_helpers.cpp) ──
extern ID3D11Device*           g_d3d_device;
extern ID3D11DeviceContext*    g_d3d_context;
extern IDXGISwapChain*         g_swap_chain;
extern ID3D11RenderTargetView* g_rtv;

// ── Custom title bar globals ──
extern HWND  g_hwnd;
extern bool  g_cursor_on_titlebar;
extern bool  g_options_menu_open;
extern ImVec2 g_gear_btn_pos;

constexpr float CUSTOM_TITLEBAR_HEIGHT = 38.0f;
constexpr float RESIZE_BORDER = 6.0f;

bool CreateD3DDevice(HWND hwnd);
void CleanupD3D();
void ResizeD3D(UINT width, UINT height);
