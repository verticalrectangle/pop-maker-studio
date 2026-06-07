#include "theme.h"
#include "app.h"
#include <imgui.h>
#include <string>
#include <cctype>

#include "inter_font.h"
#include "mono_font.h"

ImFont* g_font_regular = nullptr;
ImFont* g_font_bold    = nullptr;
ImFont* g_font_black   = nullptr;
ImFont* g_font_mono    = nullptr;

void theme_apply() {
    ImGuiIO& io = ImGui::GetIO();

    io.Fonts->Clear();

    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;

    g_font_regular = io.Fonts->AddFontFromMemoryTTF(
        (void*)inter_regular_ttf, (int)inter_regular_ttf_size, 16.f, &cfg);
    g_font_bold = io.Fonts->AddFontFromMemoryTTF(
        (void*)inter_bold_ttf, (int)inter_bold_ttf_size, 16.f, &cfg);
    g_font_black = io.Fonts->AddFontFromMemoryTTF(
        (void*)inter_black_ttf, (int)inter_black_ttf_size, 16.f, &cfg);

    // JetBrains Mono for the embedded terminal. Glyph ranges cover what fish,
    // zsh, and TUIs actually emit: extended Latin, general punctuation
    // (en/em dashes, fancy quotes, arrows like ❯), arrows, math operators,
    // box-drawing and block elements for TUIs, geometric shapes, dingbats.
    // Pointer must outlive the atlas — ImGui keeps the pointer, not a copy.
    static const ImWchar mono_ranges[] = {
        0x0020, 0x00FF, // Basic Latin + Latin-1 Supplement
        0x0100, 0x024F, // Latin Extended-A and B
        0x2000, 0x206F, // General Punctuation
        0x2070, 0x209F, // Super/Subscripts
        0x20A0, 0x20CF, // Currency Symbols
        0x2100, 0x214F, // Letterlike Symbols
        0x2150, 0x218F, // Number Forms
        0x2190, 0x21FF, // Arrows
        0x2200, 0x22FF, // Mathematical Operators
        0x2300, 0x23FF, // Misc Technical
        0x2500, 0x257F, // Box Drawing
        0x2580, 0x259F, // Block Elements
        0x25A0, 0x25FF, // Geometric Shapes
        0x2600, 0x26FF, // Misc Symbols
        0x2700, 0x27BF, // Dingbats
        0,
    };
    g_font_mono = io.Fonts->AddFontFromMemoryTTF(
        (void*)jetbrains_mono_regular_ttf, (int)jetbrains_mono_regular_ttf_size,
        28.f, &cfg, mono_ranges);

    io.Fonts->Build();

    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding    = 0.f;
    s.ChildRounding     = 2.f;
    s.FrameRounding     = 2.f;
    s.GrabRounding      = 2.f;
    s.PopupRounding     = 2.f;
    s.ScrollbarRounding = 2.f;
    s.TabRounding       = 2.f;
    s.WindowBorderSize  = 0.f;
    s.ChildBorderSize   = 1.f;
    s.FrameBorderSize   = 1.f;
    s.WindowPadding     = {8.f, 8.f};
    s.FramePadding      = {10.f, 6.f};
    s.ItemSpacing       = {8.f, 6.f};
    s.ItemInnerSpacing  = {6.f, 4.f};
    s.ScrollbarSize     = 6.f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]             = Col::bg;
    c[ImGuiCol_ChildBg]              = Col::bg_soft;
    c[ImGuiCol_PopupBg]              = {0.06f, 0.06f, 0.06f, 0.98f};
    c[ImGuiCol_Border]               = Col::line;
    c[ImGuiCol_BorderShadow]         = Col::transparent;
    c[ImGuiCol_FrameBg]              = Col::bg_soft;
    c[ImGuiCol_FrameBgHovered]       = Col::bg_soft_hov;
    c[ImGuiCol_FrameBgActive]        = Col::bg_soft_hov;
    c[ImGuiCol_TitleBg]              = Col::bg;
    c[ImGuiCol_TitleBgActive]        = Col::bg;
    c[ImGuiCol_TitleBgCollapsed]     = Col::bg;
    c[ImGuiCol_MenuBarBg]            = Col::bg;
    c[ImGuiCol_ScrollbarBg]          = Col::bg;
    c[ImGuiCol_ScrollbarGrab]        = Col::line;
    c[ImGuiCol_ScrollbarGrabHovered] = Col::line_hover;
    c[ImGuiCol_ScrollbarGrabActive]  = Col::fg;
    c[ImGuiCol_CheckMark]            = Col::fg;
    c[ImGuiCol_SliderGrab]           = Col::fg;
    c[ImGuiCol_SliderGrabActive]     = Col::fg;
    c[ImGuiCol_Button]               = Col::bg_soft;
    c[ImGuiCol_ButtonHovered]        = Col::bg_soft_hov;
    c[ImGuiCol_ButtonActive]         = Col::line;
    c[ImGuiCol_Header]               = Col::bg_soft;
    c[ImGuiCol_HeaderHovered]        = Col::bg_soft_hov;
    c[ImGuiCol_HeaderActive]         = Col::line;
    c[ImGuiCol_Separator]            = Col::line;
    c[ImGuiCol_SeparatorHovered]     = Col::line_hover;
    c[ImGuiCol_SeparatorActive]      = Col::fg;
    c[ImGuiCol_ResizeGrip]           = Col::transparent;
    c[ImGuiCol_ResizeGripHovered]    = Col::line;
    c[ImGuiCol_ResizeGripActive]     = Col::fg;
    c[ImGuiCol_Tab]                  = Col::bg_soft;
    c[ImGuiCol_TabHovered]           = Col::bg_soft_hov;
    c[ImGuiCol_TabActive]            = Col::line;
    c[ImGuiCol_TabUnfocused]         = Col::bg_soft;
    c[ImGuiCol_TabUnfocusedActive]   = Col::bg_soft;
    c[ImGuiCol_PlotLines]            = Col::muted;
    c[ImGuiCol_PlotLinesHovered]     = Col::fg;
    c[ImGuiCol_PlotHistogram]        = Col::muted;
    c[ImGuiCol_PlotHistogramHovered] = Col::fg;
    c[ImGuiCol_Text]                 = Col::fg;
    c[ImGuiCol_TextDisabled]         = Col::muted;
    c[ImGuiCol_NavHighlight]         = Col::line_hover;
    c[ImGuiCol_NavWindowingHighlight]= Col::fg;
    c[ImGuiCol_NavWindowingDimBg]    = Col::bg_soft;
    c[ImGuiCol_ModalWindowDimBg]     = {0.f, 0.f, 0.f, 0.5f};
}

void ui_label(const char* text, ImVec4 col) {
    std::string upper;
    for (const char* p = text; *p; ++p)
        upper += (char)toupper((unsigned char)*p);
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextUnformatted(upper.c_str());
    ImGui::PopStyleColor();
}

void ui_separator() {
    ImGui::PushStyleColor(ImGuiCol_Separator, Col::line);
    ImGui::Separator();
    ImGui::PopStyleColor();
}

bool ui_card_begin(const char* id, ImVec2 size, bool selected, bool hoverable) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, selected ? Col::bg_soft_hov : Col::bg_soft);
    ImGui::PushStyleColor(ImGuiCol_Border,  selected ? Col::fg : (hoverable ? Col::line : Col::line));
    bool v = ImGui::BeginChild(id, size, ImGuiChildFlags_Borders);
    ImGui::PopStyleColor(2);
    return v;
}

void ui_card_end() {
    ImGui::EndChild();
}

bool ui_btn(const char* label, bool filled, bool small) {
    float px = small ? 8.f : 16.f;
    float py = small ? 4.f : 8.f;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {px, py});
    if (filled) {
        ImGui::PushStyleColor(ImGuiCol_Button,        Col::fg);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Col::line_hover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  Col::muted);
        ImGui::PushStyleColor(ImGuiCol_Text,          Col::bg);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button,        Col::transparent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Col::bg_soft_hov);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  Col::bg_soft);
        ImGui::PushStyleColor(ImGuiCol_Text,          Col::fg);
        ImGui::PushStyleColor(ImGuiCol_Border,        Col::btn_line);
    }
    bool clicked = ImGui::Button(label);
    if (filled) ImGui::PopStyleColor(4);
    else        ImGui::PopStyleColor(5);
    ImGui::PopStyleVar();
    return clicked;
}
