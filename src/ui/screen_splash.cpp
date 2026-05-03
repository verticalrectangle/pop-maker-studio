#include "screens.h"
#include "theme.h"
#include "app.h"
#include <imgui.h>
#include <cmath>

extern ImFont* g_font_black;

void ui_splash(AppState& state) {
    ImGuiIO& io  = ImGui::GetIO();
    float    w   = io.DisplaySize.x;
    float    h   = io.DisplaySize.y;

    // Fade: full white at t=1.6, fully black at t=0
    float alpha = fminf(1.f, state.splash_timer / 0.4f);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled({0, 0}, {w, h}, IM_COL32(0, 0, 0, 255));

    // Title — Inter Black, huge
    ImGui::PushFont(g_font_black);
    ImGui::SetWindowFontScale(5.8f);

    const char* title = "POP MAKER STUDIO";
    ImVec2 tsz = ImGui::CalcTextSize(title);
    ImVec2 tpos = {(w - tsz.x) * 0.5f, (h - tsz.y) * 0.5f - 12.f};

    ImU32 col = ImGui::ColorConvertFloat4ToU32({1.f, 1.f, 1.f, alpha});
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 5.8f, tpos, col, title);

    ImGui::SetWindowFontScale(1.f);
    ImGui::PopFont();

    // Version tag bottom right — fades with title
    ImU32 dim = ImGui::ColorConvertFloat4ToU32({1.f, 1.f, 1.f, alpha * 0.25f});
    dl->AddText({w - 120.f, h - 24.f}, dim, "v0.1.0  2026");

    // Consume the full window so ImGui doesn't draw anything else
    ImGui::Dummy({w, h});
    (void)state;
}
