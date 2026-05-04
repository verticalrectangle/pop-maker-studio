#include "screens.h"
#include "app.h"
#include <imgui.h>
#include <cmath>

extern ImFont* g_font_black;

static const char* splash_status(float progress) {
    if (progress < 0.30f) return "Initializing...";
    if (progress < 0.60f) return "Loading assets...";
    if (progress < 0.85f) return "Checking for models...";
    return "Ready";
}

void ui_splash(AppState& state) {
    ImGuiIO& io = ImGui::GetIO();
    float w = io.DisplaySize.x;
    float h = io.DisplaySize.y;

    float elapsed  = fmaxf(0.f, 1.6f - state.splash_timer);
    float progress = fminf(1.f, elapsed / 1.6f);

    // Fade in over first 0.35s, fade out over last 0.2s
    float alpha;
    if (elapsed < 0.35f)
        alpha = elapsed / 0.35f;
    else if (state.splash_timer < 0.2f)
        alpha = state.splash_timer / 0.2f;
    else
        alpha = 1.f;
    alpha = fmaxf(0.f, fminf(1.f, alpha));

    auto white = [&](float a_mul = 1.f) -> ImU32 {
        return IM_COL32(255, 255, 255, (int)(255 * alpha * a_mul));
    };

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled({0.f, 0.f}, {w, h}, IM_COL32(0, 0, 0, 255));

    // ── Title ─────────────────────────────────────────────────────────────────
    const char* title = "POP MAKER STUDIO";
    ImGui::PushFont(g_font_black);
    float  base_sz  = ImGui::GetFontSize();
    ImVec2 tsz      = ImGui::CalcTextSize(title);
    float  title_sz = base_sz * ((w * 0.76f) / tsz.x);
    title_sz = fmaxf(32.f, fminf(title_sz, 180.f));
    float  sc       = title_sz / base_sz;
    float  title_cy = h * 0.42f;
    ImVec2 tpos     = {(w - tsz.x * sc) * 0.5f, title_cy - tsz.y * sc * 0.5f};
    dl->AddText(ImGui::GetFont(), title_sz, tpos, white(), title);
    ImGui::PopFont();

    // ── Subtitle (2× default size) ────────────────────────────────────────────
    const char* sub  = "Shortform Video Editor";
    float  sub_sz    = ImGui::GetFontSize() * 2.f;
    float  sub_scale = sub_sz / ImGui::GetFontSize();
    ImVec2 ssz1      = ImGui::CalcTextSize(sub);
    float  sub_y     = tpos.y + tsz.y * sc + 14.f;
    dl->AddText(ImGui::GetFont(), sub_sz,
                {(w - ssz1.x * sub_scale) * 0.5f, sub_y},
                white(), sub);

    // ── Bottom strip ──────────────────────────────────────────────────────────
    float  lbl_sz    = ImGui::GetFontSize() * 2.f;   // 2× for publisher
    float  lbl_scale = lbl_sz / ImGui::GetFontSize();
    float  bar_y     = h - 2.f;
    float  label_y   = bar_y - 6.f - ImGui::GetFontSize() * lbl_scale;
    float  status_y  = label_y - 6.f - ImGui::GetFontSize();

    // Bar track + fill
    dl->AddRectFilled({0.f, bar_y}, {w, h},            white(0.11f));
    dl->AddRectFilled({0.f, bar_y}, {w * progress, h}, white(0.78f));

    // Status text (1×) — bottom left
    dl->AddText({16.f, status_y}, white(), splash_status(progress));

    // Publisher (2×) — bottom left, Inter Black
    ImGui::PushFont(g_font_black);
    dl->AddText(ImGui::GetFont(), lbl_sz, {16.f, label_y}, white(), "VERTICAL RECTANGLE");
    ImGui::PopFont();

    // Version (1×) — bottom right
    const char* ver = "v0.1.0  2026";
    ImVec2 vsz = ImGui::CalcTextSize(ver);
    dl->AddText({w - vsz.x - 16.f, label_y + (ImGui::GetFontSize() * lbl_scale - ImGui::GetFontSize()) * 0.5f},
                white(), ver);

    ImGui::Dummy({w, h});
}
