#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "app_state.h"
#include "connection.h"
#include "d3d_helpers.h"
#include "persistence.h"
#include "screen_threads.h"
#include "theme.h"
#include "update_checker.h"

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

#include <d3d11.h>
#include <windows.h>
#include <tchar.h>

#include <mfapi.h>

#include <dwmapi.h>
#include <windowsx.h>
#include <shellapi.h>

#include <thread>
#include <regex>
#include <iostream>

// Validate IP address format (IPv4)
bool is_valid_ip(const std::string& ip) {
    const std::regex ip_regex(
        R"(^((25[0-5]|2[0-4][0-9]|[0-1]?[0-9][0-9]?)\.){3}(25[0-5]|2[0-4][0-9]|[0-1]?[0-9][0-9]?)$)");
    return std::regex_match(ip, ip_regex);
}

// Validate username (alphanumeric, 3-32 characters)
bool is_valid_username(const std::string& username) {
    const std::regex username_regex(R"(^[a-zA-Z0-9_]{3,32}$)");
    return std::regex_match(username, username_regex);
}

// Sanitize chat input (remove control characters)
std::string sanitize_chat_input(const std::string& input) {
    std::string sanitized;
    for (char c : input) {
        if (std::isprint(static_cast<unsigned char>(c)) || std::isspace(static_cast<unsigned char>(c))) {
            sanitized += c;
        }
    }
    return sanitized;
}

// ── Forward declarations ──
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_NCCALCSIZE: {
        if (wParam == TRUE) {
            auto& params = *reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
            // When maximized, constrain to the monitor working area so we
            // don't cover the taskbar.
            if (IsZoomed(hwnd)) {
                MONITORINFO mi = { sizeof(mi) };
                GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
                params.rgrc[0] = mi.rcWork;
            }
            return 0; // removes the native title bar
        }
        break;
    }
    case WM_NCHITTEST: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        RECT rc;
        GetWindowRect(hwnd, &rc);

        bool maximized = IsZoomed(hwnd);

        // Resize borders (skip when maximized)
        if (!maximized) {
            bool left   = pt.x < rc.left   + (int)RESIZE_BORDER;
            bool right  = pt.x >= rc.right  - (int)RESIZE_BORDER;
            bool top    = pt.y < rc.top     + (int)RESIZE_BORDER;
            bool bottom = pt.y >= rc.bottom - (int)RESIZE_BORDER;

            if (top && left)     return HTTOPLEFT;
            if (top && right)    return HTTOPRIGHT;
            if (bottom && left)  return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left)            return HTLEFT;
            if (right)           return HTRIGHT;
            if (top)             return HTTOP;
            if (bottom)          return HTBOTTOM;
        }

        // Title bar area -- enable drag (but not over traffic light buttons)
        if (pt.y < rc.top + (int)CUSTOM_TITLEBAR_HEIGHT && !g_cursor_on_titlebar) {
            return HTCAPTION;
        }

        return HTCLIENT;
    }
    case WM_NCACTIVATE:
        // Prevent default title bar redraw
        return TRUE;
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        mmi->ptMinTrackSize.x = 700;
        mmi->ptMinTrackSize.y = 450;
        return 0;
    }
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED)
            ResizeD3D(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ════════════════════════════════════════════════════════════════
//  WinMain
// ════════════════════════════════════════════════════════════════

int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPSTR, _In_ int) {
    (void)CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);
    lilypad::WinsockInit winsock;
    lilypad::PortAudioInit pa;

    WNDCLASSEX wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_CLASSDC;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance      = hInstance;
    wc.lpszClassName  = _T("LilyPadClient");
    wc.hCursor        = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassEx(&wc);

    g_hwnd = CreateWindow(wc.lpszClassName, _T("LilyPad Voice Chat"),
        WS_OVERLAPPEDWINDOW, 100, 100, 900, 620,
        nullptr, nullptr, wc.hInstance, nullptr);

    // Extend frame into client area to preserve DWM drop shadow
    MARGINS margins = {0, 0, 0, 1};
    DwmExtendFrameIntoClientArea(g_hwnd, &margins);

    // Force WM_NCCALCSIZE immediately so the native title bar is never visible
    SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    if (!CreateD3DDevice(g_hwnd)) {
        CleanupD3D();
        UnregisterClass(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ShowWindow(g_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(g_hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ApplyLilyPadTheme();

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_d3d_device, g_d3d_context);

    // ── State ──
    AppState app;

    // Check for updates from GitHub in a background thread
    std::thread(check_for_update_thread, std::ref(app)).detach();

    char ip_buf[64]       = "127.0.0.1";
    char username_buf[64] = "";
    char chat_input[512]  = "";

    auto input_devices  = lilypad::get_input_devices();
    auto output_devices = lilypad::get_output_devices();

    int selected_input  = -1;
    int selected_output = -1;

    int default_in  = lilypad::get_default_input_device();
    int default_out = lilypad::get_default_output_device();
    for (int i = 0; i < static_cast<int>(input_devices.size()); ++i) {
        if (input_devices[i].index == default_in) { selected_input = i; break; }
    }
    for (int i = 0; i < static_cast<int>(output_devices.size()); ++i) {
        if (output_devices[i].index == default_out) { selected_output = i; break; }
    }

    // PTT UI state
    bool ptt_enabled = false;
    int  ptt_key_sel = 0; // index into g_ptt_keys
    bool noise_suppression = true;

    // Screen sharing UI state
    int bitrate_mbps = 0;  // 0 = auto

    bool scroll_chat_to_bottom = true;

    // Server favorites
    auto favorites = load_favorites();
    int  selected_fav = -1;
    char fav_name_buf[64] = "";

    // Settings (auto-connect)
    auto settings = load_settings();
    bool auto_connect = settings.auto_connect;
    bool auto_connect_pending = false;

    // Pre-fill last server info if auto-connect is enabled
    if (auto_connect && !settings.last_server_ip.empty()) {
        strncpy(ip_buf, settings.last_server_ip.c_str(), sizeof(ip_buf) - 1);
        ip_buf[sizeof(ip_buf) - 1] = '\0';
        if (!settings.last_username.empty()) {
            strncpy(username_buf, settings.last_username.c_str(), sizeof(username_buf) - 1);
            username_buf[sizeof(username_buf) - 1] = '\0';
        }
        auto_connect_pending = true;
    }

    // ── Main loop ──
    ImVec4 clear_color = ImVec4(0.06f, 0.06f, 0.08f, 1.00f);
    bool running = true;

    while (running) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                running = false;
        }
        if (!running) break;

        // Auto-connect to last server on first frame
        if (auto_connect_pending) {
            auto_connect_pending = false;
            if (!settings.last_username.empty()) {
                do_connect(app, ip_buf, username_buf);
            }
        }

        // Poll PTT key state
        if (app.ptt_enabled) {
            bool held = (GetAsyncKeyState(app.ptt_key.load()) & 0x8000) != 0;
            app.ptt_active = held;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // ── Screen viewer SRV comes directly from H.264 decoder (no upload needed) ──

        // ── Full-window panel ──
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("##Main", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoBringToFrontOnFocus);

        // ── Custom title bar with traffic light buttons ──
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 wp = ImGui::GetWindowPos();
            float win_w = ImGui::GetWindowWidth();

            // Background
            dl->AddRectFilled(wp, ImVec2(wp.x + win_w, wp.y + CUSTOM_TITLEBAR_HEIGHT),
                              IM_COL32(20, 20, 24, 255));

            // Separator line at bottom of title bar
            dl->AddLine(ImVec2(wp.x, wp.y + CUSTOM_TITLEBAR_HEIGHT),
                        ImVec2(wp.x + win_w, wp.y + CUSTOM_TITLEBAR_HEIGHT),
                        IM_COL32(60, 60, 68, 255));

            // Traffic light buttons (right side: minimize, maximize, close)
            const float btn_radius  = 5.5f;
            const float btn_spacing = 22.0f;
            const float btn_margin_right = 18.0f;
            const float btn_center_y = CUSTOM_TITLEBAR_HEIGHT * 0.5f;

            // Rightmost button (close) is at win_w - margin - radius, then go left
            ImVec2 btn_centers[3] = {
                ImVec2(wp.x + win_w - btn_margin_right,                       wp.y + btn_center_y), // close (rightmost)
                ImVec2(wp.x + win_w - btn_margin_right - btn_spacing,         wp.y + btn_center_y), // maximize
                ImVec2(wp.x + win_w - btn_margin_right - btn_spacing * 2.0f,  wp.y + btn_center_y), // minimize
            };

            // Button colors: close (red), maximize (green), minimize (yellow)
            ImU32 btn_colors_active[3] = {
                IM_COL32(0xFF, 0x5F, 0x57, 255),  // close - red
                IM_COL32(0x28, 0xC8, 0x40, 255),  // maximize - green
                IM_COL32(0xFE, 0xBC, 0x2E, 255),  // minimize - yellow
            };
            ImU32 btn_color_inactive = IM_COL32(75, 75, 80, 255); // gray when unfocused

            bool window_focused = (GetForegroundWindow() == g_hwnd);

            // Gear button position (left of minimize)
            ImVec2 gear_center(wp.x + win_w - btn_margin_right - btn_spacing * 3.0f, wp.y + btn_center_y);
            g_gear_btn_pos = gear_center;

            // Check if mouse is hovering the button group area (traffic lights + gear)
            ImVec2 group_min(wp.x + win_w - btn_margin_right - btn_spacing * 3.0f - btn_radius - 4.0f,
                             wp.y + btn_center_y - btn_radius - 4.0f);
            ImVec2 group_max(wp.x + win_w - btn_margin_right + btn_radius + 4.0f,
                             wp.y + btn_center_y + btn_radius + 4.0f);
            ImVec2 mouse = ImGui::GetMousePos();
            bool group_hovered = (mouse.x >= group_min.x && mouse.x <= group_max.x &&
                                  mouse.y >= group_min.y && mouse.y <= group_max.y);

            // Track if cursor is over traffic light buttons (for WM_NCHITTEST)
            g_cursor_on_titlebar = group_hovered;

            // Draw buttons
            for (int i = 0; i < 3; i++) {
                ImU32 color = window_focused ? btn_colors_active[i] : btn_color_inactive;
                dl->AddCircleFilled(btn_centers[i], btn_radius, color);
            }

            // Draw icons on hover (only when group is hovered)
            if (group_hovered) {
                ImU32 icon_col = IM_COL32(60, 20, 20, 200);
                float s = 3.0f; // icon half-size

                // Close icon (X) -- index 0, rightmost
                dl->AddLine(ImVec2(btn_centers[0].x - s, btn_centers[0].y - s),
                            ImVec2(btn_centers[0].x + s, btn_centers[0].y + s),
                            icon_col, 1.5f);
                dl->AddLine(ImVec2(btn_centers[0].x + s, btn_centers[0].y - s),
                            ImVec2(btn_centers[0].x - s, btn_centers[0].y + s),
                            icon_col, 1.5f);

                // Maximize icon -- index 1, middle
                if (IsZoomed(g_hwnd)) {
                    // Restore icon: two small arrows pointing inward
                    dl->AddLine(ImVec2(btn_centers[1].x - s, btn_centers[1].y + s),
                                ImVec2(btn_centers[1].x,      btn_centers[1].y),
                                icon_col, 1.5f);
                    dl->AddLine(ImVec2(btn_centers[1].x + s, btn_centers[1].y - s),
                                ImVec2(btn_centers[1].x,      btn_centers[1].y),
                                icon_col, 1.5f);
                } else {
                    dl->AddLine(ImVec2(btn_centers[1].x - s, btn_centers[1].y + s),
                                ImVec2(btn_centers[1].x + s, btn_centers[1].y - s),
                                icon_col, 1.5f);
                }

                // Minimize icon (dash) -- index 2, leftmost
                dl->AddLine(ImVec2(btn_centers[2].x - s, btn_centers[2].y),
                            ImVec2(btn_centers[2].x + s, btn_centers[2].y),
                            icon_col, 1.5f);

                // Handle clicks
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    for (int i = 0; i < 3; i++) {
                        float dx = mouse.x - btn_centers[i].x;
                        float dy = mouse.y - btn_centers[i].y;
                        if (dx * dx + dy * dy <= btn_radius * btn_radius) {
                            if (i == 0) PostMessage(g_hwnd, WM_CLOSE, 0, 0);
                            if (i == 1) ShowWindow(g_hwnd, IsZoomed(g_hwnd) ? SW_RESTORE : SW_MAXIMIZE);
                            if (i == 2) ShowWindow(g_hwnd, SW_MINIMIZE);
                            break;
                        }
                    }
                }
            }

            // Gear (options) button
            {
                float gr = btn_radius; // same radius as traffic lights
                bool gear_hovered = false;
                {
                    float dx = mouse.x - gear_center.x;
                    float dy = mouse.y - gear_center.y;
                    gear_hovered = (dx * dx + dy * dy <= (gr + 4.0f) * (gr + 4.0f));
                }

                ImU32 gear_color = g_options_menu_open
                    ? IM_COL32(180, 180, 190, 255)
                    : (gear_hovered ? IM_COL32(140, 140, 150, 255) : IM_COL32(90, 90, 100, 255));

                // Draw gear: outer circle + inner circle + 6 teeth (short radial lines)
                dl->AddCircle(gear_center, gr, gear_color, 0, 1.5f);
                dl->AddCircleFilled(gear_center, gr * 0.35f, gear_color);
                const int teeth = 6;
                for (int t = 0; t < teeth; t++) {
                    float angle = (float)t / (float)teeth * 6.2831853f;
                    float cos_a = cosf(angle);
                    float sin_a = sinf(angle);
                    dl->AddLine(
                        ImVec2(gear_center.x + cos_a * (gr - 1.0f), gear_center.y + sin_a * (gr - 1.0f)),
                        ImVec2(gear_center.x + cos_a * (gr + 2.5f), gear_center.y + sin_a * (gr + 2.5f)),
                        gear_color, 2.0f);
                }

                // Click to toggle options menu
                if (gear_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    g_options_menu_open = !g_options_menu_open;
                }
            }

            // Title text (left side)
            float text_x = 14.0f;
            float text_y = btn_center_y - ImGui::GetFontSize() * 0.5f;
            dl->AddText(ImVec2(wp.x + text_x, wp.y + text_y),
                        IM_COL32(84, 184, 122, 255), "LilyPad");
            float lilypad_w = ImGui::CalcTextSize("LilyPad").x;
            dl->AddText(ImVec2(wp.x + text_x + lilypad_w + 6.0f, wp.y + text_y),
                        IM_COL32(128, 128, 138, 255), "Voice Chat");

            // Status indicator on the right
            if (app.connected && app.in_voice) {
                const char* status_text = nullptr;
                ImU32 status_color = 0;
                if (app.muted) {
                    status_text = "[MUTED]";
                    status_color = IM_COL32(184, 71, 71, 255);
                } else if (app.ptt_enabled) {
                    if (app.ptt_active) {
                        status_text = "[TRANSMITTING]";
                        status_color = IM_COL32(102, 209, 140, 255);
                    } else {
                        static char ptt_buf[64];
                        snprintf(ptt_buf, sizeof(ptt_buf), "[PTT: %s]", g_ptt_keys[ptt_key_sel].name);
                        status_text = ptt_buf;
                        status_color = IM_COL32(128, 128, 138, 255);
                    }
                }
                if (status_text) {
                    float tw = ImGui::CalcTextSize(status_text).x;
                    float buttons_left = win_w - btn_margin_right - btn_spacing * 2.0f - btn_radius - 14.0f;
                    dl->AddText(ImVec2(wp.x + buttons_left - tw, wp.y + text_y),
                                status_color, status_text);
                }
            }

            // Advance cursor past the title bar area
            ImGui::SetCursorPosY(CUSTOM_TITLEBAR_HEIGHT + 4.0f);
        }

        // ── Options dropdown menu (anchored below gear button) ──
        if (g_options_menu_open) {
            ImGui::SetNextWindowPos(ImVec2(g_gear_btn_pos.x - 200.0f, g_gear_btn_pos.y + 18.0f));
            ImGui::SetNextWindowSize(ImVec2(240, 0));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.12f, 0.12f, 0.14f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.24f, 0.24f, 0.27f, 1.0f));
            ImGui::Begin("##OptionsMenu", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoSavedSettings);

            ImGui::TextColored(ImVec4(0.33f, 0.72f, 0.48f, 1.0f), "Audio Devices");
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::Text("Input");
            ImGui::SetNextItemWidth(-1);
            {
                const char* in_preview = (selected_input >= 0 && selected_input < static_cast<int>(input_devices.size()))
                    ? input_devices[selected_input].name.c_str() : "Default";
                if (ImGui::BeginCombo("##opt_in_dev", in_preview)) {
                    for (int i = 0; i < static_cast<int>(input_devices.size()); ++i) {
                        bool sel = (selected_input == i);
                        if (ImGui::Selectable(input_devices[i].name.c_str(), sel))
                            selected_input = i;
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }

            ImGui::Spacing();
            ImGui::Text("Output");
            ImGui::SetNextItemWidth(-1);
            {
                const char* out_preview = (selected_output >= 0 && selected_output < static_cast<int>(output_devices.size()))
                    ? output_devices[selected_output].name.c_str() : "Default";
                if (ImGui::BeginCombo("##opt_out_dev", out_preview)) {
                    for (int i = 0; i < static_cast<int>(output_devices.size()); ++i) {
                        bool sel = (selected_output == i);
                        if (ImGui::Selectable(output_devices[i].name.c_str(), sel))
                            selected_output = i;
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }

            ImGui::Spacing();
            ImGui::Spacing();

            ImGui::TextColored(ImVec4(0.33f, 0.72f, 0.48f, 1.0f), "Voice Mode");
            ImGui::Separator();
            ImGui::Spacing();

            if (ImGui::Checkbox("Push-to-Talk##opt", &ptt_enabled)) {
                if (app.in_voice.load()) app.ptt_enabled = ptt_enabled;
            }
            if (ptt_enabled) {
                ImGui::Text("PTT Key");
                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo("##opt_ptt_key", g_ptt_keys[ptt_key_sel].name)) {
                    for (int i = 0; i < g_ptt_key_count; ++i) {
                        bool sel = (ptt_key_sel == i);
                        if (ImGui::Selectable(g_ptt_keys[i].name, sel)) {
                            ptt_key_sel = i;
                            if (app.in_voice.load()) app.ptt_key = g_ptt_keys[i].vk;
                        }
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }

            ImGui::Spacing();
            if (ImGui::Checkbox("Noise Suppression##opt", &noise_suppression)) {
                if (app.in_voice.load()) app.noise_suppression = noise_suppression;
            }

            ImGui::Spacing();

            // Close when clicking outside the popup
            if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem | ImGuiHoveredFlags_ChildWindows) &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                // Don't close if clicking the gear button itself (toggle handles that)
                float dx = ImGui::GetMousePos().x - g_gear_btn_pos.x;
                float dy = ImGui::GetMousePos().y - g_gear_btn_pos.y;
                if (dx * dx + dy * dy > 100.0f) {
                    g_options_menu_open = false;
                }
            }

            ImGui::End();
            ImGui::PopStyleColor(2);
        }

        bool is_connected = app.connected.load();

        // ════════════════════════════════════════
        //  Left panel: Connection / Users / Settings
        // ════════════════════════════════════════
        float panel_width = 260.0f;
        ImGui::BeginChild("##LeftPanel", ImVec2(panel_width, 0), true);

        ImGui::TextColored(ImVec4(0.33f, 0.72f, 0.48f, 1.0f), "Connection");
        ImGui::Separator();
        ImGui::Spacing();

        if (!is_connected) {
            ImGui::Text("Server IP");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##ip", ip_buf, sizeof(ip_buf) - 1);

            // Validate IP address before connecting
            if (!is_valid_ip(ip_buf)) {
                std::cerr << "Invalid IP address: " << ip_buf << "\n";
                strncpy(ip_buf, "127.0.0.1", sizeof(ip_buf) - 1); // Reset to default
                ip_buf[sizeof(ip_buf) - 1] = '\0';
            }

            // Validate username before connecting
            if (!is_valid_username(username_buf)) {
                std::cerr << "Invalid username: " << username_buf << "\n";
                strncpy(username_buf, "Guest", sizeof(username_buf) - 1); // Reset to default
                username_buf[sizeof(username_buf) - 1] = '\0';
            }

            // Display error messages for invalid input
            if (!is_valid_ip(ip_buf)) {
                ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Invalid IP address.");
            }
            if (!is_valid_username(username_buf)) {
                ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Invalid username. Use 3-32 alphanumeric characters.");
            }

            // Favorites
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.33f, 0.72f, 0.48f, 1.0f), "Favorites");
            ImGui::Separator();
            ImGui::Spacing();

            // Favorite server buttons -- click to connect, X to remove
            int fav_to_remove = -1;
            for (int i = 0; i < static_cast<int>(favorites.size()); ++i) {
                ImGui::PushID(i);

                // X remove button (small, red)
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.50f, 0.18f, 0.18f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.72f, 0.28f, 0.28f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.82f, 0.33f, 0.33f, 1.0f));
                if (ImGui::SmallButton("X")) {
                    fav_to_remove = i;
                }
                ImGui::PopStyleColor(3);

                ImGui::SameLine();

                // Connect button (full width, green)
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.55f, 0.38f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.33f, 0.72f, 0.48f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.40f, 0.82f, 0.55f, 1.0f));
                if (ImGui::Button(favorites[i].name.c_str(), ImVec2(-1, 0))) {
                    // Fill fields and connect
                    strncpy(ip_buf, favorites[i].ip.c_str(), sizeof(ip_buf) - 1);
                    ip_buf[sizeof(ip_buf) - 1] = '\0';
                    if (!favorites[i].username.empty()) {
                        strncpy(username_buf, favorites[i].username.c_str(), sizeof(username_buf) - 1);
                        username_buf[sizeof(username_buf) - 1] = '\0';
                    }
                    app.ptt_enabled = ptt_enabled;
                    app.ptt_key     = g_ptt_keys[ptt_key_sel].vk;
                    app.noise_suppression = noise_suppression;
                    do_connect(app, favorites[i].ip, favorites[i].username.empty() ? std::string(username_buf) : favorites[i].username);

                    if (app.connected.load()) {
                        settings.last_server_ip = ip_buf;
                        settings.last_username = username_buf;
                        save_settings(settings);
                    }
                }
                ImGui::PopStyleColor(3);

                // Tooltip with IP on hover
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", favorites[i].ip.c_str());
                }

                ImGui::PopID();
            }
            if (fav_to_remove >= 0) {
                favorites.erase(favorites.begin() + fav_to_remove);
                selected_fav = -1;
                save_favorites(favorites);
            }

            // Save current IP to favorites
            ImGui::Spacing();
            ImGui::Text("Name");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##fav_name", fav_name_buf, sizeof(fav_name_buf));
            if (ImGui::SmallButton("Save to Favorites")) {
                // Validate and sanitize favorite server details
                if (!is_valid_ip(ip_buf)) {
                    std::cerr << "Invalid IP address for favorite: " << ip_buf << "\n";
                    return;
                }
                std::string sanitized_name = sanitize_chat_input(fav_name_buf);
                if (sanitized_name.empty()) {
                    sanitized_name = ip_buf; // Default to IP if name is invalid
                }
                favorites.push_back({sanitized_name, ip_buf, username_buf});
                save_favorites(favorites);
            }

            ImGui::Spacing();
            ImGui::Spacing();

            ImGui::Text("Username");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##user", username_buf, sizeof(username_buf) - 1);

            ImGui::Spacing();
            ImGui::Spacing();

            // Update available button (pre-connection)
            if (app.update_available.load()) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70f, 0.55f, 0.15f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.68f, 0.20f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.95f, 0.78f, 0.25f, 1.0f));
                char update_label[128];
                {
                    std::lock_guard<std::mutex> lk(app.update_mutex);
                    snprintf(update_label, sizeof(update_label), "Update Available (%s)", app.update_version.c_str());
                }
                if (ImGui::Button(update_label, ImVec2(-1, 30))) {
                    std::lock_guard<std::mutex> lk(app.update_mutex);
                    ShellExecuteA(nullptr, "open", app.update_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
                ImGui::PopStyleColor(3);
                ImGui::Spacing();
            }

            // Auto-connect toggle
            if (ImGui::Checkbox("Auto-connect to last server", &auto_connect)) {
                settings.auto_connect = auto_connect;
                save_settings(settings);
            }

            ImGui::Spacing();

            // Connect button
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.55f, 0.38f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.33f, 0.72f, 0.48f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.40f, 0.82f, 0.55f, 1.0f));
            if (ImGui::Button("Connect", ImVec2(-1, 36))) {
                app.ptt_enabled = ptt_enabled;
                app.ptt_key     = g_ptt_keys[ptt_key_sel].vk;
                app.noise_suppression = noise_suppression;
                do_connect(app, ip_buf, username_buf);

                // Save last server for auto-connect
                if (app.connected.load()) {
                    settings.last_server_ip = ip_buf;
                    settings.last_username = username_buf;
                    save_settings(settings);
                }
            }
            ImGui::PopStyleColor(3);

        } else {
            // ── Connected state ──
            ImGui::TextColored(ImVec4(0.33f, 0.72f, 0.48f, 1.0f), "Connected");
            ImGui::Text("Server: %s", ip_buf);
            ImGui::Text("Your ID: %u", app.my_id);

            ImGui::Spacing();

            bool is_in_voice = app.in_voice.load();

            // Join / Leave Voice button
            if (is_in_voice) {
                // Leave Voice (red)
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.22f, 0.22f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.72f, 0.28f, 0.28f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.82f, 0.33f, 0.33f, 1.0f));
                if (ImGui::Button("Leave Voice", ImVec2(-1, 30))) {
                    do_leave_voice(app);
                }
                ImGui::PopStyleColor(3);
            } else {
                // Join Voice (green)
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.55f, 0.38f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.33f, 0.72f, 0.48f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.40f, 0.82f, 0.55f, 1.0f));
                if (ImGui::Button("Join Voice", ImVec2(-1, 30))) {
                    app.ptt_enabled = ptt_enabled;
                    app.ptt_key     = g_ptt_keys[ptt_key_sel].vk;
                    app.noise_suppression = noise_suppression;
                    int in_dev  = (selected_input >= 0 && selected_input < static_cast<int>(input_devices.size()))
                        ? input_devices[selected_input].index : -1;
                    int out_dev = (selected_output >= 0 && selected_output < static_cast<int>(output_devices.size()))
                        ? output_devices[selected_output].index : -1;
                    do_join_voice(app, in_dev, out_dev);
                }
                ImGui::PopStyleColor(3);
            }

            ImGui::Spacing();

            // Mute button and voice settings (only when in voice)
            if (is_in_voice) {
                {
                    bool is_muted = app.muted.load();
                    if (is_muted) {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.22f, 0.22f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.72f, 0.28f, 0.28f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.82f, 0.33f, 0.33f, 1.0f));
                    } else {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.55f, 0.38f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.33f, 0.72f, 0.48f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.40f, 0.82f, 0.55f, 1.0f));
                    }
                    if (ImGui::Button(is_muted ? "Unmute" : "Mute", ImVec2(-1, 30))) {
                        app.muted = !is_muted;
                    }
                    ImGui::PopStyleColor(3);
                }

            }

            ImGui::Spacing();
            ImGui::Spacing();

            // Screen sharing
            ImGui::TextColored(ImVec4(0.33f, 0.72f, 0.48f, 1.0f), "Screen Sharing");
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::Text("Bitrate");
            ImGui::SetNextItemWidth(-1);
            if (bitrate_mbps == 0) {
                // Show current auto value
                int cur = app.h264_bitrate.load();
                char auto_label[32];
                snprintf(auto_label, sizeof(auto_label), "Auto (%d Mbps)", cur / 1000000);
                ImGui::TextDisabled("%s", auto_label);
            }
            if (ImGui::SliderInt("##bitrate", &bitrate_mbps, 0, 50, bitrate_mbps == 0 ? "Auto" : "%d Mbps")) {
                if (bitrate_mbps > 0) {
                    app.h264_bitrate.store(bitrate_mbps * 1000000);
                } else {
                    app.h264_bitrate.store(0);  // reset to auto on next share
                }
            }

            ImGui::Spacing();

            if (app.screen_sharing.load()) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.22f, 0.22f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.72f, 0.28f, 0.28f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.82f, 0.33f, 0.33f, 1.0f));
                if (ImGui::Button("Stop Sharing", ImVec2(-1, 30))) {
                    app.screen_sharing = false;
                    app.screen_send_cv.notify_all(); // wake send thread to exit
                    // Wait for threads to finish
                    if (app.screen_thread && app.screen_thread->joinable())
                        app.screen_thread->join();
                    app.screen_thread.reset();
                    if (app.sys_audio_thread && app.sys_audio_thread->joinable())
                        app.sys_audio_thread->join();
                    app.sys_audio_thread.reset();
                    if (app.screen_send_thread && app.screen_send_thread->joinable())
                        app.screen_send_thread->join();
                    app.screen_send_thread.reset();
                    // Clear any leftover items in the queue
                    {
                        std::lock_guard<std::mutex> lk(app.screen_send_mutex);
                        app.screen_send_queue.clear();
                    }
                    app.send_tcp(lilypad::make_screen_stop_msg());
                }
                ImGui::PopStyleColor(3);
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.40f, 0.55f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.52f, 0.70f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.33f, 0.60f, 0.80f, 1.0f));
                if (ImGui::Button("Share Screen", ImVec2(-1, 30))) {
                    app.screen_sharing = true;
                    app.send_tcp(lilypad::make_screen_start_msg());
                    app.screen_send_thread = std::make_unique<std::thread>(screen_send_thread_func, std::ref(app));
                    app.screen_thread = std::make_unique<std::thread>(screen_capture_thread_func, std::ref(app));
                    app.sys_audio_thread = std::make_unique<std::thread>(sys_audio_capture_thread_func, std::ref(app));
                }
                ImGui::PopStyleColor(3);
            }

            ImGui::Spacing();
            ImGui::Spacing();

            // User list -- grouped by voice channel / text chat
            {
                std::lock_guard<std::mutex> lk(app.users_mutex);

                // Lambda to render a user entry
                auto render_user = [&](const UserEntry& u, bool show_voice_indicator) {
                    ImGui::PushID(static_cast<int>(u.id));

                    if (show_voice_indicator) {
                        // Talking indicator (only for voice users)
                        bool is_talking = false;
                        {
                            std::lock_guard<std::mutex> va_lk(app.voice_activity_mutex);
                            auto va_it = app.voice_last_seen.find(u.id);
                            if (va_it != app.voice_last_seen.end()) {
                                auto elapsed = std::chrono::steady_clock::now() - va_it->second;
                                is_talking = elapsed < std::chrono::milliseconds(300);
                            }
                        }
                        float vol = app.get_volume(u.id);
                        ImVec4 dot_color;
                        if (vol < 0.01f) {
                            dot_color = ImVec4(0.55f, 0.22f, 0.22f, 1.0f); // muted (red)
                        } else if (is_talking) {
                            dot_color = ImVec4(0.40f, 0.82f, 0.55f, 1.0f); // talking (bright green)
                        } else {
                            dot_color = ImVec4(0.35f, 0.35f, 0.38f, 1.0f); // idle (gray)
                        }
                        ImGui::TextColored(dot_color, "  *");
                    } else {
                        // Online dot (text-only users)
                        ImGui::TextColored(ImVec4(0.33f, 0.72f, 0.48f, 1.0f), "  *");
                    }
                    ImGui::SameLine();
                    ImGui::Text("%s", u.name.c_str());
                    ImGui::SameLine();
                    ImGui::TextDisabled("(#%u)", u.id);

                    // Screen sharing Watch/Stop button
                    if (u.is_sharing) {
                        ImGui::SameLine();
                        uint32_t watching = app.watching_user_id.load();
                        if (watching == u.id) {
                            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.22f, 0.22f, 1.0f));
                            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.72f, 0.28f, 0.28f, 1.0f));
                            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.82f, 0.33f, 0.33f, 1.0f));
                            if (ImGui::SmallButton("Stop")) {
                                app.send_tcp(lilypad::make_screen_unsubscribe_msg(u.id));
                                app.watching_user_id = 0;
                            }
                            ImGui::PopStyleColor(3);
                        } else {
                            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.40f, 0.55f, 1.0f));
                            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.52f, 0.70f, 1.0f));
                            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.33f, 0.60f, 0.80f, 1.0f));
                            if (ImGui::SmallButton("Watch")) {
                                if (watching != 0) {
                                    app.send_tcp(lilypad::make_screen_unsubscribe_msg(watching));
                                }
                                app.watching_user_id = u.id;
                                app.send_tcp(lilypad::make_screen_subscribe_msg(u.id));
                            }
                            ImGui::PopStyleColor(3);
                        }
                    }

                    // Right-click context menu for volume
                    if (show_voice_indicator) {
                        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                            ImGui::OpenPopup("##vol_popup");
                        }
                        if (ImGui::BeginPopup("##vol_popup")) {
                            float vol = app.get_volume(u.id);
                            ImGui::Text("Volume: %s", u.name.c_str());
                            ImGui::Separator();
                            float vol_pct = vol * 100.0f;
                            ImGui::SetNextItemWidth(180);
                            if (ImGui::SliderFloat("##vol", &vol_pct, 0.0f, 200.0f, "%.0f%%",
                                    ImGuiSliderFlags_AlwaysClamp)) {
                                app.set_volume(u.id, vol_pct / 100.0f);
                            }
                            if (ImGui::Button("Reset to 100%", ImVec2(-1, 0))) {
                                app.set_volume(u.id, 1.0f);
                            }
                            ImGui::EndPopup();
                        }
                    }

                    ImGui::PopID();
                };

                // Voice Channel group
                ImGui::TextColored(ImVec4(0.33f, 0.72f, 0.48f, 1.0f), "Voice Channel");
                ImGui::Separator();
                ImGui::Spacing();

                bool any_voice = false;
                for (auto& u : app.users) {
                    if (u.in_voice) {
                        render_user(u, true);
                        any_voice = true;
                    }
                }
                if (!any_voice) {
                    ImGui::TextDisabled("  No users in voice.");
                }

                ImGui::Spacing();
                ImGui::Spacing();

                // Text Chat group
                ImGui::TextColored(ImVec4(0.33f, 0.72f, 0.48f, 1.0f), "Text Chat");
                ImGui::Separator();
                ImGui::Spacing();

                bool any_text = false;
                for (auto& u : app.users) {
                    if (!u.in_voice) {
                        render_user(u, false);
                        any_text = true;
                    }
                }
                if (!any_text) {
                    ImGui::TextDisabled("  No text-only users.");
                }
            }

            ImGui::Spacing();
            ImGui::Spacing();

            // Disconnect
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.22f, 0.22f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.72f, 0.28f, 0.28f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.82f, 0.33f, 0.33f, 1.0f));
            if (ImGui::Button("Disconnect", ImVec2(-1, 36))) {
                do_disconnect(app);
            }
            ImGui::PopStyleColor(3);
        }

        ImGui::EndChild();

        ImGui::SameLine();

        // ════════════════════════════════════════
        //  Right panel: Chat
        // ════════════════════════════════════════
        ImGui::BeginChild("##RightPanel", ImVec2(0, 0), true);

        ImGui::TextColored(ImVec4(0.33f, 0.72f, 0.48f, 1.0f), "Chat");
        ImGui::Separator();
        ImGui::Spacing();

        // Chat messages area (fill available height minus input bar)
        float input_height = 40.0f;
        ImGui::BeginChild("##ChatScroll", ImVec2(0, -input_height), false);
        {
            std::lock_guard<std::mutex> lk(app.chat_mutex);
            for (auto& m : app.chat_messages) {
                if (m.is_system) {
                    ImGui::TextDisabled("  %s", m.text.c_str());
                } else {
                    // Sender name in accent color
                    bool is_me = (m.sender_id == app.my_id);
                    ImVec4 name_color = is_me
                        ? ImVec4(0.55f, 0.75f, 0.95f, 1.0f)
                        : ImVec4(0.33f, 0.72f, 0.48f, 1.0f);
                    ImGui::TextColored(name_color, "%s:", m.sender_name.c_str());
                    ImGui::SameLine();
                    ImGui::TextWrapped("%s", m.text.c_str());
                }
            }
        }
        if (scroll_chat_to_bottom && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20.0f)
            ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();

        // Chat input bar
        ImGui::Separator();
        bool send_chat = false;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 70);
        if (ImGui::InputText("##chat_input", chat_input, sizeof(chat_input) - 1,
                ImGuiInputTextFlags_EnterReturnsTrue)) {
            send_chat = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Send", ImVec2(-1, 0))) {
            send_chat = true;
        }

        // Sanitize chat input before sending
        if (send_chat && chat_input[0] != '\0' && is_connected) {
            std::string sanitized_input = sanitize_chat_input(chat_input);
            if (!sanitized_input.empty()) {
                auto msg = lilypad::make_text_chat_msg(sanitized_input);
                app.send_tcp(msg);
            } else {
                std::cerr << "Chat input contains invalid characters.\n";
            }
            chat_input[0] = '\0';
            scroll_chat_to_bottom = true;
            ImGui::SetKeyboardFocusHere(-1); // Re-focus the input field
        }

        ImGui::EndChild();

        ImGui::End(); // Main window

        // ── Screen viewer floating window ──
        uint32_t watching_id = app.watching_user_id.load();
        {
            ID3D11ShaderResourceView* srv = nullptr;
            int srv_w = 0, srv_h = 0;
            {
                std::lock_guard<std::mutex> lk(app.screen_srv_mutex);
                srv   = app.screen_srv;
                srv_w = app.screen_srv_w;
                srv_h = app.screen_srv_h;
            }
            if (watching_id != 0 && srv && srv_w > 0 && srv_h > 0) {
                std::string watcher_name = app.lookup_username(watching_id);
                std::string win_title = "Screen: " + watcher_name;

                ImGui::SetNextWindowSize(ImVec2(640, 400), ImGuiCond_FirstUseEver);
                bool viewer_open = true;
                if (ImGui::Begin(win_title.c_str(), &viewer_open, ImGuiWindowFlags_NoCollapse)) {
                    ImVec2 avail = ImGui::GetContentRegionAvail();
                    float aspect = static_cast<float>(srv_w) / static_cast<float>(srv_h);
                    float disp_w = avail.x;
                    float disp_h = avail.x / aspect;
                    if (disp_h > avail.y) {
                        disp_h = avail.y;
                        disp_w = avail.y * aspect;
                    }
                    float offset_x = (avail.x - disp_w) * 0.5f;
                    if (offset_x > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset_x);
                    ImGui::Image((ImTextureID)(uintptr_t)srv, ImVec2(disp_w, disp_h));

                    // Right-click context menu
                    if (ImGui::BeginPopupContextWindow("##stream_ctx")) {
                        ImGui::Text("Stream Volume");
                        ImGui::SetNextItemWidth(150);
                        int vol_pct = static_cast<int>(app.stream_volume * 100.0f + 0.5f);
                        if (ImGui::SliderInt("##stream_vol", &vol_pct, 0, 200, "%d%%")) {
                            app.stream_volume = static_cast<float>(vol_pct) / 100.0f;
                        }
                        ImGui::EndPopup();
                    }
                }
                ImGui::End();

                if (!viewer_open) {
                    app.send_tcp(lilypad::make_screen_unsubscribe_msg(watching_id));
                    app.watching_user_id = 0;
                }
            }
        }

        // ── Render ─
        ImGui::Render();
        g_d3d_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        float clear[4] = { clear_color.x, clear_color.y, clear_color.z, clear_color.w };
        g_d3d_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap_chain->Present(1, 0);
    }

    // ── Cleanup ──
    do_disconnect(app);
    app.running = false;

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupD3D();
    DestroyWindow(g_hwnd);
    UnregisterClass(wc.lpszClassName, wc.hInstance);

    MFShutdown();
    CoUninitialize();

    return 0;
}
