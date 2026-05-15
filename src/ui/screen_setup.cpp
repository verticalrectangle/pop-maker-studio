#include "screens.h"
#include "theme.h"
#include "../app.h"
#include "../paths.h"
#include <imgui.h>
#include <string>
#include <vector>
#include <filesystem>

extern ImFont* g_font_bold;

namespace fs = std::filesystem;

// ── Model presence check ──────────────────────────────────────────────────────

struct ModelEntry {
    std::string filename;
    std::string label;
};

static const ModelEntry kRequiredModels[] = {
    { "ggml-large-v3-turbo-q5_0.bin", "Whisper ggml-large-v3-turbo"   },
    { "Kim_Vocal_2.onnx",             "Kim_Vocal_2 MDX vocal model"    },
    { "u2net_human_seg.onnx",         "u2net human segmentation model" },
    { "hubert.onnx",                  "HuBERT voice features model"    },
};

static std::vector<std::string> missing_models() {
    std::string mdir = app_models_dir();
    std::vector<std::string> missing;
    for (auto& m : kRequiredModels) {
        if (!fs::exists(mdir + "/" + m.filename))
            missing.push_back(m.label);
    }
    return missing;
}

// ── Setup screen ──────────────────────────────────────────────────────────────

void ui_setup(AppState& state) {
    // On first entry: check models and auto-advance if all present
    static bool s_checked = false;
    if (!s_checked) {
        s_checked = true;
        if (missing_models().empty()) {
            state.models_ready  = true;
            state.models_skipped = true;
            return;
        }
    }

    // If already advancing, skip drawing
    if (state.models_skipped) return;

    ImGuiIO& io = ImGui::GetIO();
    float W = io.DisplaySize.x, H = io.DisplaySize.y;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled({0,0},{W,H}, to_u32(Col::bg));

    auto missing = missing_models();

    const float cw = 520.f;
    const float ch = 80.f + (float)missing.size() * 24.f + 80.f;
    ImGui::SetCursorPos({(W-cw)*0.5f, (H-ch)*0.5f});

    ImGui::PushStyleColor(ImGuiCol_ChildBg, to_u32(Col::bg_soft));
    ImGui::PushStyleColor(ImGuiCol_Border,  to_u32(Col::line));
    ImGui::BeginChild("##setup_card", {cw, ch}, ImGuiChildFlags_Borders);

    float iw  = ImGui::GetContentRegionAvail().x;
    float pad = ImGui::GetStyle().WindowPadding.x + 16.f;

    ImGui::Dummy({0.f, 20.f});

    const char* title = "Missing Models";
    ImGui::SetCursorPosX((iw - ImGui::CalcTextSize(title).x) * 0.5f
                          + ImGui::GetStyle().WindowPadding.x);
    ImGui::PushFont(g_font_bold);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    ImGui::Dummy({0.f, 10.f});

    ImGui::SetCursorPosX(pad);
    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
    ImGui::TextUnformatted("Place the models/ folder next to the binary.");
    ImGui::PopStyleColor();
    ImGui::Dummy({0.f, 8.f});

    for (auto& name : missing) {
        ImGui::SetCursorPosX(pad);
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255,120,80,255));
        ImGui::TextUnformatted(("  \xe2\x9c\x97  " + name).c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Dummy({0.f, 16.f});

    float bw = 160.f;
    ImGui::SetCursorPosX((iw - bw) * 0.5f + ImGui::GetStyle().WindowPadding.x);
    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
    if (ImGui::Button("Continue anyway", {bw, 28.f}))
        state.models_skipped = true;
    ImGui::PopStyleColor();

    ImGui::EndChild();
    ImGui::PopStyleColor(2);
}

// ── Re-download modal — no-op (models are bundled) ───────────────────────────

void ui_model_download_modal(AppState& state) {
    // Models are bundled — nothing to download.
    (void)state;
}
