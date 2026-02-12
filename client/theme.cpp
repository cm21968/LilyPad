#include "theme.h"
#include <imgui.h>

void ApplyLilyPadTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors    = style.Colors;

    style.WindowRounding    = 6.0f;
    style.FrameRounding     = 4.0f;
    style.GrabRounding      = 3.0f;
    style.PopupRounding     = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.TabRounding       = 4.0f;
    style.WindowPadding     = ImVec2(12, 12);
    style.FramePadding      = ImVec2(8, 5);
    style.ItemSpacing       = ImVec2(8, 6);

    ImVec4 bg_dark   = ImVec4(0.08f, 0.08f, 0.10f, 1.00f);
    ImVec4 bg_mid    = ImVec4(0.12f, 0.12f, 0.14f, 1.00f);
    ImVec4 bg_light  = ImVec4(0.18f, 0.18f, 0.21f, 1.00f);
    ImVec4 accent    = ImVec4(0.33f, 0.72f, 0.48f, 1.00f);
    ImVec4 accent_hi = ImVec4(0.40f, 0.82f, 0.55f, 1.00f);
    ImVec4 accent_lo = ImVec4(0.25f, 0.55f, 0.38f, 1.00f);
    ImVec4 text      = ImVec4(0.90f, 0.92f, 0.90f, 1.00f);
    ImVec4 text_dim  = ImVec4(0.55f, 0.57f, 0.55f, 1.00f);
    ImVec4 border    = ImVec4(0.22f, 0.22f, 0.25f, 1.00f);

    colors[ImGuiCol_WindowBg]             = bg_dark;
    colors[ImGuiCol_ChildBg]              = bg_mid;
    colors[ImGuiCol_PopupBg]              = ImVec4(0.10f, 0.10f, 0.12f, 0.96f);
    colors[ImGuiCol_Border]               = border;
    colors[ImGuiCol_FrameBg]              = bg_light;
    colors[ImGuiCol_FrameBgHovered]       = ImVec4(0.24f, 0.24f, 0.28f, 1.00f);
    colors[ImGuiCol_FrameBgActive]        = ImVec4(0.28f, 0.28f, 0.33f, 1.00f);
    colors[ImGuiCol_TitleBg]              = bg_dark;
    colors[ImGuiCol_TitleBgActive]        = bg_mid;
    colors[ImGuiCol_TitleBgCollapsed]     = bg_dark;
    colors[ImGuiCol_MenuBarBg]            = bg_mid;
    colors[ImGuiCol_ScrollbarBg]          = bg_dark;
    colors[ImGuiCol_ScrollbarGrab]        = bg_light;
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.28f, 0.28f, 0.32f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]  = accent_lo;
    colors[ImGuiCol_CheckMark]            = accent;
    colors[ImGuiCol_SliderGrab]           = accent;
    colors[ImGuiCol_SliderGrabActive]     = accent_hi;
    colors[ImGuiCol_Button]               = bg_light;
    colors[ImGuiCol_ButtonHovered]        = ImVec4(0.25f, 0.25f, 0.30f, 1.00f);
    colors[ImGuiCol_ButtonActive]         = accent_lo;
    colors[ImGuiCol_Header]               = bg_light;
    colors[ImGuiCol_HeaderHovered]        = ImVec4(0.24f, 0.24f, 0.28f, 1.00f);
    colors[ImGuiCol_HeaderActive]         = accent_lo;
    colors[ImGuiCol_Separator]            = border;
    colors[ImGuiCol_SeparatorHovered]     = accent;
    colors[ImGuiCol_SeparatorActive]      = accent_hi;
    colors[ImGuiCol_ResizeGrip]           = ImVec4(0.20f, 0.20f, 0.23f, 0.50f);
    colors[ImGuiCol_ResizeGripHovered]    = accent;
    colors[ImGuiCol_ResizeGripActive]     = accent_hi;
    colors[ImGuiCol_Tab]                  = bg_light;
    colors[ImGuiCol_TabHovered]           = accent;
    colors[ImGuiCol_TabSelected]          = accent_lo;
    colors[ImGuiCol_Text]                 = text;
    colors[ImGuiCol_TextDisabled]         = text_dim;
}
