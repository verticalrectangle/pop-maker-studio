#include "screens.h"
#include "theme.h"
#include "../app.h"
#include "../globals.h"
#include <imgui.h>
#include <cstdio>
#include <cstring>
#include <thread>
#include <atomic>
#include <string>
#include <mutex>

// ── Download worker ───────────────────────────────────────────────────────────

static std::atomic<bool> s_dl_thread_running{false};
static std::mutex        s_dl_mutex;
static std::string       s_dl_stage;
static std::string       s_dl_message;
static float             s_dl_progress = 0.f;
static bool              s_dl_done     = false;
static bool              s_dl_error    = false;
static std::string       s_dl_error_msg;

static void download_worker(std::string python, std::string script) {
    {
        std::lock_guard<std::mutex> lk(s_dl_mutex);
        s_dl_stage    = "whisper";
        s_dl_message  = "Starting…";
        s_dl_progress = 0.f;
        s_dl_done     = false;
        s_dl_error    = false;
        s_dl_error_msg.clear();
    }

    std::string cmd = "\"" + python + "\" \"" + script + "\" 2>&1";
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) {
        std::lock_guard<std::mutex> lk(s_dl_mutex);
        s_dl_error     = true;
        s_dl_error_msg = "Failed to launch prefetch script";
        s_dl_thread_running = false;
        return;
    }

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        // strip newline
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = 0;

        std::lock_guard<std::mutex> lk(s_dl_mutex);
        std::string l(line);
        if (l.rfind("STAGE:", 0) == 0) {
            s_dl_stage = l.substr(6);
            s_dl_progress = (s_dl_stage == "demucs") ? 0.5f : 0.f;
        } else if (l.rfind("OK:", 0) == 0) {
            s_dl_progress = (l.substr(3) == "whisper") ? 0.5f : 1.f;
        } else if (l.rfind("ERROR:", 0) == 0) {
            // ERROR:<model>:<message>
            auto sep = l.find(':', 6);
            s_dl_error     = true;
            s_dl_error_msg = (sep != std::string::npos) ? l.substr(sep + 1) : l.substr(6);
        } else if (l == "DONE") {
            s_dl_done     = true;
            s_dl_progress = 1.f;
        } else {
            s_dl_message = l;
        }
    }
    pclose(fp);
    s_dl_thread_running = false;
}

// ── Setup screen ──────────────────────────────────────────────────────────────

void ui_setup(AppState& state) {
    // Sync download worker state into AppState each frame
    if (state.model_dl_running) {
        {
            std::lock_guard<std::mutex> lk(s_dl_mutex);
            state.model_dl_stage    = s_dl_stage;
            state.model_dl_message  = s_dl_message;
            state.model_dl_progress = s_dl_progress;
            state.model_dl_error    = s_dl_error;
            state.model_dl_error_msg= s_dl_error_msg;
            if (s_dl_done) {
                state.model_dl_done    = true;
                state.model_dl_running = false;
                state.models_ready     = true;
            }
            if (s_dl_error && !s_dl_thread_running) {
                state.model_dl_running = false;
            }
        }
    }

    ImGuiIO& io = ImGui::GetIO();
    float W = io.DisplaySize.x;
    float H = io.DisplaySize.y;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled({0,0},{W,H}, to_u32(Col::bg));

    // Center card
    const float cw = 480.f;
    const float ch = state.model_dl_running ? 260.f : 320.f;
    float cx = (W - cw) * 0.5f;
    float cy = (H - ch) * 0.5f;

    ImGui::SetCursorPos({cx, cy});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, to_u32(Col::bg_soft));
    ImGui::PushStyleColor(ImGuiCol_Border,  to_u32(Col::line));
    ImGui::BeginChild("##setup_card", {cw, ch}, ImGuiChildFlags_Borders);

    ImGui::Dummy({0.f, 24.f});
    float iw = ImGui::GetContentRegionAvail().x;

    // Title
    const char* title = "Lyric Extraction Models";
    float tw = ImGui::CalcTextSize(title).x;
    ImGui::SetCursorPosX((iw - tw) * 0.5f + ImGui::GetStyle().WindowPadding.x);
    ImGui::PushStyleColor(ImGuiCol_Text, Col::fg);
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();

    ImGui::Dummy({0.f, 12.f});

    if (!state.model_dl_running && !state.model_dl_done && !state.model_dl_error) {
        // ── Prompt ────────────────────────────────────────────────────────────
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::SetNextItemWidth(iw - 32.f);
        ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x + 16.f);
        ImGui::TextWrapped(
            "Pop Maker Studio can separate vocals and transcribe lyrics "
            "using on-device AI. This requires downloading ~400 MB of "
            "model weights once — they stay on your machine permanently.");
        ImGui::PopStyleColor();

        ImGui::Dummy({0.f, 24.f});

        // Buttons
        float bw = 200.f, bh = 32.f;
        float bx = (iw - bw * 2.f - 12.f) * 0.5f + ImGui::GetStyle().WindowPadding.x;

        ImGui::SetCursorPosX(bx);
        ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(255,255,255,220));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  IM_COL32(255,255,255,255));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,   IM_COL32(200,200,200,255));
        ImGui::PushStyleColor(ImGuiCol_Text,           IM_COL32(0,0,0,255));
        if (ImGui::Button("Download Models", {bw, bh})) {
            state.model_dl_running = true;
            state.model_dl_done    = false;
            state.model_dl_error   = false;
            s_dl_done  = false;
            s_dl_error = false;
            s_dl_thread_running = true;
            std::thread(download_worker,
                state.python_path, g_prefetch_script).detach();
        }
        ImGui::PopStyleColor(4);

        ImGui::SameLine(0.f, 12.f);
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        if (ImGui::Button("Skip for now", {bw, bh}))
            state.models_skipped = true;
        ImGui::PopStyleColor();

    } else if (state.model_dl_running) {
        // ── Download progress ─────────────────────────────────────────────────
        const char* stage_label = (state.model_dl_stage == "demucs")
            ? "Downloading Demucs vocal separator…"
            : "Downloading Whisper transcription model…";

        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        float lw = ImGui::CalcTextSize(stage_label).x;
        ImGui::SetCursorPosX((iw - lw) * 0.5f + ImGui::GetStyle().WindowPadding.x);
        ImGui::TextUnformatted(stage_label);
        ImGui::PopStyleColor();

        ImGui::Dummy({0.f, 12.f});

        float pw = iw - 64.f;
        ImVec2 bp = {ImGui::GetCursorScreenPos().x + (iw - pw) * 0.5f,
                     ImGui::GetCursorScreenPos().y};
        ImGui::GetWindowDrawList()->AddRectFilled(bp, {bp.x+pw, bp.y+4.f},
            to_u32(Col::line), 2.f);
        ImGui::GetWindowDrawList()->AddRectFilled(bp,
            {bp.x + pw * state.model_dl_progress, bp.y+4.f},
            to_u32(Col::fg), 2.f);
        ImGui::Dummy({0.f, 8.f});

        if (!state.model_dl_message.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
            float mw = ImGui::CalcTextSize(state.model_dl_message.c_str()).x;
            ImGui::SetCursorPosX((iw - fminf(mw, iw-32.f)) * 0.5f
                                  + ImGui::GetStyle().WindowPadding.x);
            ImGui::TextUnformatted(state.model_dl_message.c_str());
            ImGui::PopStyleColor();
        }

    } else if (state.model_dl_error) {
        // ── Error ─────────────────────────────────────────────────────────────
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255,80,80,255));
        ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x + 16.f);
        ImGui::TextWrapped("Download failed: %s", state.model_dl_error_msg.c_str());
        ImGui::PopStyleColor();

        ImGui::Dummy({0.f, 16.f});
        float bw = 160.f;
        ImGui::SetCursorPosX((iw - bw) * 0.5f + ImGui::GetStyle().WindowPadding.x);
        if (ui_btn("Try again", false, false)) {
            state.model_dl_running  = true;
            state.model_dl_error    = false;
            s_dl_done  = false;
            s_dl_error = false;
            s_dl_thread_running = true;
            std::thread(download_worker,
                state.python_path, g_prefetch_script).detach();
        }

    } else if (state.model_dl_done) {
        // ── Success ───────────────────────────────────────────────────────────
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        const char* msg = "Models ready. You're all set.";
        float mw = ImGui::CalcTextSize(msg).x;
        ImGui::SetCursorPosX((iw - mw) * 0.5f + ImGui::GetStyle().WindowPadding.x);
        ImGui::TextUnformatted(msg);
        ImGui::PopStyleColor();

        ImGui::Dummy({0.f, 16.f});
        float bw = 160.f;
        ImGui::SetCursorPosX((iw - bw) * 0.5f + ImGui::GetStyle().WindowPadding.x);
        if (ui_btn("Open Studio", true, false))
            state.models_skipped = true;  // just lets app.cpp fall through to ui_studio
    }

    ImGui::EndChild();
    ImGui::PopStyleColor(2);
}

// ── Download modal (triggered from Help menu in studio) ───────────────────────

void ui_model_download_modal(AppState& state) {
    if (!state.show_model_dl_modal) return;

    ImGui::OpenPopup("##model_dl_modal");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({460.f, 0.f});
    ImGui::PushStyleColor(ImGuiCol_PopupBg, to_u32(Col::bg_soft));
    ImGui::PushStyleColor(ImGuiCol_Border,  to_u32(Col::line));

    if (ImGui::BeginPopupModal("##model_dl_modal", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize)) {

        // Sync worker state
        if (state.model_dl_running) {
            std::lock_guard<std::mutex> lk(s_dl_mutex);
            state.model_dl_stage    = s_dl_stage;
            state.model_dl_message  = s_dl_message;
            state.model_dl_progress = s_dl_progress;
            state.model_dl_error    = s_dl_error;
            state.model_dl_error_msg= s_dl_error_msg;
            if (s_dl_done) {
                state.model_dl_done    = true;
                state.model_dl_running = false;
                state.models_ready     = true;
            }
            if (s_dl_error && !s_dl_thread_running)
                state.model_dl_running = false;
        }

        ImGui::Dummy({0.f, 12.f});
        ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x + 8.f);
        ImGui::PushStyleColor(ImGuiCol_Text, Col::fg);
        ImGui::TextUnformatted("Download Lyric Extraction Models");
        ImGui::PopStyleColor();
        ImGui::Dummy({0.f, 8.f});

        if (!state.model_dl_running && !state.model_dl_done) {
            ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x + 8.f);
            ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
            if (state.model_dl_error)
                ImGui::TextWrapped("Previous attempt failed: %s\n\nClick Download to retry.",
                    state.model_dl_error_msg.c_str());
            else
                ImGui::TextWrapped("Downloads Whisper large-v2 and Demucs htdemucs (~400 MB). "
                    "Stored permanently in ~/.cache.");
            ImGui::PopStyleColor();
            ImGui::Dummy({0.f, 12.f});

            if (ui_btn("Download", true, false)) {
                state.model_dl_running = true;
                state.model_dl_error   = false;
                s_dl_done  = false;
                s_dl_error = false;
                s_dl_thread_running = true;
                std::thread(download_worker,
                    state.python_path, g_prefetch_script).detach();
            }
            ImGui::SameLine(0.f, 8.f);
            if (ui_btn("Cancel", false, false)) {
                state.show_model_dl_modal = false;
                ImGui::CloseCurrentPopup();
            }
        } else if (state.model_dl_running) {
            const char* stage_label = (state.model_dl_stage == "demucs")
                ? "Downloading Demucs vocal separator…"
                : "Downloading Whisper transcription model…";
            ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x + 8.f);
            ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
            ImGui::TextUnformatted(stage_label);
            ImGui::PopStyleColor();
            ImGui::Dummy({0.f, 8.f});

            float pw = ImGui::GetContentRegionAvail().x - 16.f;
            ImVec2 bp = ImGui::GetCursorScreenPos();
            bp.x += 8.f;
            ImGui::GetWindowDrawList()->AddRectFilled(bp, {bp.x+pw, bp.y+4.f},
                to_u32(Col::line), 2.f);
            ImGui::GetWindowDrawList()->AddRectFilled(bp,
                {bp.x + pw * state.model_dl_progress, bp.y+4.f},
                to_u32(Col::fg), 2.f);
            ImGui::Dummy({0.f, 16.f});
        } else if (state.model_dl_done) {
            ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x + 8.f);
            ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
            ImGui::TextUnformatted("Models installed successfully.");
            ImGui::PopStyleColor();
            ImGui::Dummy({0.f, 12.f});
            if (ui_btn("Close", false, false)) {
                state.show_model_dl_modal = false;
                ImGui::CloseCurrentPopup();
            }
        }

        ImGui::Dummy({0.f, 8.f});
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor(2);
}
