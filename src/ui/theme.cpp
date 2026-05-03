#include "theme.h"
#include "app.h"
#include <imgui.h>
#include <string>
#include <cctype>

// Fonts loaded from inter_font.h (generated at build time)
#include "inter_font.h"

ImFont* g_font_regular = nullptr;
ImFont* g_font_bold    = nullptr;

void theme_apply() {
    ImGuiIO& io = ImGui::GetIO();

    io.Fonts->Clear();

    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;

    g_font_regular = io.Fonts->AddFontFromMemoryTTF(
        (void*)inter_regular_ttf, (int)inter_regular_ttf_size, 14.f, &cfg);
    g_font_bold = io.Fonts->AddFontFromMemoryTTF(
        (void*)inter_bold_ttf, (int)inter_bold_ttf_size, 14.f, &cfg);

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
    s.WindowPadding     = {32.f, 32.f};
    s.FramePadding      = {10.f, 6.f};
    s.ItemSpacing       = {8.f, 6.f};
    s.ItemInnerSpacing  = {6.f, 4.f};
    s.ScrollbarSize     = 6.f;

    // Map every ImGui color to the B&W palette
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]             = Col::bg;
    c[ImGuiCol_ChildBg]              = Col::bg_soft;
    c[ImGuiCol_PopupBg]              = Col::bg;
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

// ── Shared UI helpers ─────────────────────────────────────────────────────────

void ui_label(const char* text, ImVec4 col) {
    // Uppercase + tight letter spacing approximated by inter-character spacing
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
    ImGui::PushStyleColor(ImGuiCol_ChildBg,    selected ? Col::bg_soft_hov : Col::bg_soft);
    ImGui::PushStyleColor(ImGuiCol_Border,     selected ? Col::fg : (hoverable ? Col::line : Col::line));
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

// ── Top bar ───────────────────────────────────────────────────────────────────

static const char* SCREEN_LABELS[] = {
    "01  Start", "02  Upload", "03  Lyrics", "04  Style", "05  Export"
};

void ui_topbar(AppState& state) {
    float bar_h = 48.f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 sz = {ImGui::GetContentRegionAvail().x, bar_h};

    // Background
    dl->AddRectFilled(p, {p.x + sz.x, p.y + sz.y}, to_u32(Col::bg));
    // Bottom border
    dl->AddLine({p.x, p.y + sz.y}, {p.x + sz.x, p.y + sz.y}, to_u32(Col::line));

    ImGui::SetCursorScreenPos({p.x + 32.f, p.y + 14.f});
    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
    ImGui::TextUnformatted("POP MAKER  /  STUDIO");
    ImGui::PopStyleColor();

    // Nav buttons centered
    float btn_w = 110.f;
    float total_w = btn_w * 5.f;
    float start_x = p.x + (sz.x - total_w) * 0.5f;

    for (int i = 0; i < 5; ++i) {
        bool active = (int)state.current_screen == i;
        ImGui::SetCursorScreenPos({start_x + i * btn_w, p.y});
        ImGui::PushStyleColor(ImGuiCol_Button,        active ? Col::bg_soft_hov : Col::transparent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Col::bg_soft_hov);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  Col::bg_soft_hov);
        ImGui::PushStyleColor(ImGuiCol_Text,          active ? Col::fg : Col::muted);
        ImGui::PushStyleColor(ImGuiCol_Border,        Col::line);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0.f, 0.f});

        char nav_id[32];
        snprintf(nav_id, sizeof(nav_id), "##nav%d", i);
        if (ImGui::Button(nav_id, {btn_w, bar_h}))
            state.go((Screen)i);

        // Label drawn manually centered in the button
        ImVec2 lsz = ImGui::CalcTextSize(SCREEN_LABELS[i]);
        ImVec2 lpos = {start_x + i * btn_w + (btn_w - lsz.x) * 0.5f,
                       p.y + (bar_h - lsz.y) * 0.5f};
        dl->AddText(lpos, to_u32(active ? Col::fg : Col::muted), SCREEN_LABELS[i]);

        // Active underline
        if (active)
            dl->AddLine({start_x + i * btn_w, p.y + bar_h - 1.f},
                        {start_x + i * btn_w + btn_w, p.y + bar_h - 1.f},
                        to_u32(Col::fg));

        ImGui::PopStyleColor(5);
        ImGui::PopStyleVar();
    }

    // Right side — session indicator
    ImGui::SetCursorScreenPos({p.x + sz.x - 160.f, p.y + 14.f});
    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
    ImGui::TextUnformatted("SESSION LIVE");
    ImGui::PopStyleColor();

    // Advance cursor past the topbar
    ImGui::SetCursorScreenPos({p.x, p.y + bar_h + 1.f});
    ImGui::Dummy({0.f, 0.f});
}
