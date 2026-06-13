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
        20.f, &cfg, mono_ranges);

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

// A small round "+" button pinned to a card's top-right corner — the explicit
// "add at playhead" action, sitting on top of the (drag-source) card body.
// Call right after the card's InvisibleButton + drag-source block; the caller
// must have called ImGui::SetNextItemAllowOverlap() before the card so this
// button can receive clicks over it. `uid` keeps the button id unique.
bool ui_card_add_btn(ImVec2 card_tl, float card_w, int uid) {
    const float sz = 20.f, pad = 5.f;
    // The "+" is an overlay anchored to the card's top-right. Save and restore
    // the layout cursor around it — otherwise SetCursorScreenPos clobbers the
    // advance the card's InvisibleButton already made, and the next card stacks
    // on top of this one (cards end up overlapping by their lower half).
    ImVec2 save = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos({card_tl.x + card_w - sz - pad, card_tl.y + pad});
    ImGui::PushID(uid);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0.f, 0.f});
    ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(235, 120, 60, 235));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(255, 150, 80, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(210, 100, 45, 255));
    ImGui::PushStyleColor(ImGuiCol_Text,          IM_COL32(20, 16, 12, 255));
    bool clicked = ImGui::Button("+##cardadd", {sz, sz});
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Add at playhead");
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
    ImGui::PopID();
    ImGui::SetCursorScreenPos(save);
    return clicked;
}

// ── Shared library-card hover-dwell clock + image popover ────────────────────
static int    s_card_hover_id = -1;
static double s_card_hover_t0 = 0.0;

float ui_card_hover_secs(int card_id, bool hovered) {
    if (hovered) {
        if (s_card_hover_id != card_id) {
            s_card_hover_id = card_id;
            s_card_hover_t0 = ImGui::GetTime();
        }
        return (float)(ImGui::GetTime() - s_card_hover_t0);
    }
    if (s_card_hover_id == card_id) s_card_hover_id = -1;
    return 0.f;
}

bool ui_card_hover_ready(int card_id, float dwell_secs) {
    return s_card_hover_id == card_id &&
           (float)(ImGui::GetTime() - s_card_hover_t0) >= dwell_secs;
}

void ui_card_image_popover(ImVec2 card_tl, ImTextureID tex,
                           float img_w, float img_h, bool flip,
                           const char* name, const char* subtitle) {
    if (!tex || img_w <= 0.f || img_h <= 0.f) return;
    const float maxw = 240.f, maxh = 390.f, pad = 9.f;
    float txt_h = (subtitle ? 42.f : 26.f);
    float iw = maxw, ih = iw * (img_h / img_w);
    if (ih > maxh) { ih = maxh; iw = ih * (img_w / img_h); }

    ImGuiViewport* vp = ImGui::GetMainViewport();
    float x1 = card_tl.x - 14.f, x0 = x1 - iw;
    float minx = vp->Pos.x + 8.f;
    if (x0 < minx) { x0 = minx; x1 = x0 + iw; }
    float box_h = ih + txt_h + pad;
    float y0 = card_tl.y - 6.f;
    float maxy = vp->Pos.y + vp->Size.y - 8.f;
    if (y0 + box_h + pad > maxy) y0 = maxy - box_h - pad;
    if (y0 < vp->Pos.y + 8.f) y0 = vp->Pos.y + 8.f;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled({x0 - pad, y0 - pad}, {x1 + pad, y0 + box_h + pad},
                      IM_COL32(14, 14, 20, 252), 9.f);
    dl->AddRect({x0 - pad, y0 - pad}, {x1 + pad, y0 + box_h + pad},
                IM_COL32(90, 90, 120, 255), 9.f, 0, 1.5f);
    ImVec2 uv0 = flip ? ImVec2(0, 1) : ImVec2(0, 0);
    ImVec2 uv1 = flip ? ImVec2(1, 0) : ImVec2(1, 1);
    dl->AddImageRounded(tex, {x0, y0}, {x0 + iw, y0 + ih}, uv0, uv1, IM_COL32_WHITE, 6.f);
    dl->AddRect({x0, y0}, {x0 + iw, y0 + ih}, IM_COL32(255, 255, 255, 40), 6.f);
    if (name) {
        ImGui::PushFont(g_font_bold);
        dl->AddText(ImGui::GetFont(), 15.f, {x0, y0 + ih + 7.f},
                    IM_COL32(255, 255, 255, 245), name);
        ImGui::PopFont();
    }
    if (subtitle)
        dl->AddText({x0, y0 + ih + 26.f}, IM_COL32(160, 160, 175, 220), subtitle);
}
