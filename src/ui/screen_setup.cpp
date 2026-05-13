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
#include <functional>
#include <filesystem>

extern ImFont* g_font_bold;

namespace fs = std::filesystem;

// ── Shared worker state ───────────────────────────────────────────────────────

static std::atomic<bool> s_worker_running{false};
static std::mutex        s_worker_mutex;
static AppState::SetupStage s_stage   = AppState::SetupStage::Idle;
static std::string          s_message;
static float                s_progress = 0.f;
static bool                 s_done     = false;
static bool                 s_error    = false;
static std::string          s_error_msg;

static void worker_set(AppState::SetupStage stage, float progress,
                        const std::string& msg = "") {
    std::lock_guard<std::mutex> lk(s_worker_mutex);
    s_stage    = stage;
    s_progress = progress;
    if (!msg.empty()) s_message = msg;
}
static void worker_err(const std::string& msg) {
    std::lock_guard<std::mutex> lk(s_worker_mutex);
    s_error     = true;
    s_error_msg = msg;
    s_worker_running = false;
}

static bool run_cmd(const std::string& cmd,
                    std::function<void(const std::string&)> on_line) {
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) return false;
    char buf[1024];
    while (fgets(buf, sizeof(buf), fp)) {
        size_t len = strlen(buf);
        while (len > 0 && (buf[len-1]=='\n'||buf[len-1]=='\r')) buf[--len]=0;
        on_line(std::string(buf));
    }
    return pclose(fp) == 0;
}

static std::string find_song2subs_python() {
    static const char* alts[] = {
        "/home/alexis/dev/song2subs/venv/bin/python3",
        nullptr
    };
    for (const char** a = alts; *a; a++)
        if (fs::exists(*a)) return *a;
    return "";
}

// ── Model download worker ─────────────────────────────────────────────────────

static void model_dl_worker(std::string prefetch_script) {
    std::string py = find_song2subs_python();
    if (py.empty()) {
        return worker_err("song2subs venv not found at /home/alexis/dev/song2subs/venv");
    }

    worker_set(AppState::SetupStage::ModelDL, 0.f, "Starting model download…");

    std::string cmd = "\"" + py + "\" \"" + prefetch_script + "\" 2>&1";
    run_cmd(cmd, [](const std::string& line) {
        std::lock_guard<std::mutex> lk(s_worker_mutex);
        if (line.rfind("STAGE:", 0) == 0) {
            std::string stg = line.substr(6);
            s_progress = (stg == "demucs") ? 0.5f : 0.f;
            s_stage    = AppState::SetupStage::ModelDL;
            s_message  = (stg == "demucs") ? "Downloading Demucs htdemucs…"
                                            : "Downloading faster-whisper large-v3…";
        } else if (line.rfind("OK:", 0) == 0) {
            s_progress = (line.substr(3) == "whisper") ? 0.5f : 1.f;
        } else if (line.rfind("ERROR:", 0) == 0) {
            auto sep = line.find(':', 6);
            s_error     = true;
            s_error_msg = (sep != std::string::npos) ? line.substr(sep+1) : line.substr(6);
        } else if (line == "DONE") {
            s_done     = true;
            s_progress = 1.f;
            s_stage    = AppState::SetupStage::Done;
        } else {
            s_message = line;
        }
    });

    if (!s_error && !s_done) {
        std::lock_guard<std::mutex> lk(s_worker_mutex);
        s_error     = true;
        s_error_msg = "Model download did not complete.";
    }
    s_worker_running = false;
}

// ── Shared progress bar helper ────────────────────────────────────────────────

static void draw_progress_bar(float progress, float width) {
    ImVec2 bp = ImGui::GetCursorScreenPos();
    bp.x += 8.f;
    float pw = width - 16.f;
    ImGui::GetWindowDrawList()->AddRectFilled(bp, {bp.x+pw, bp.y+4.f},
        to_u32(Col::line), 2.f);
    ImGui::GetWindowDrawList()->AddRectFilled(bp,
        {bp.x + pw * fmaxf(0.f, fminf(1.f, progress)), bp.y+4.f},
        to_u32(Col::fg), 2.f);
    ImGui::Dummy({0.f, 12.f});
}

// ── Setup screen ──────────────────────────────────────────────────────────────

void ui_setup(AppState& state) {
    if (state.setup_running) {
        std::lock_guard<std::mutex> lk(s_worker_mutex);
        state.setup_stage    = s_stage;
        state.setup_progress = s_progress;
        state.setup_message  = s_message;
        if (s_error) {
            state.setup_stage     = AppState::SetupStage::Error;
            state.setup_error_msg = s_error_msg;
            state.setup_running   = false;
        } else if (s_done) {
            state.setup_stage   = AppState::SetupStage::Done;
            state.setup_running = false;
            state.models_ready  = true;
        }
    }

    ImGuiIO& io = ImGui::GetIO();
    float W = io.DisplaySize.x, H = io.DisplaySize.y;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled({0,0},{W,H}, to_u32(Col::bg));

    const float cw = 500.f;
    bool is_running = state.setup_running ||
        state.setup_stage == AppState::SetupStage::Done ||
        state.setup_stage == AppState::SetupStage::Error;
    const float ch = is_running ? 240.f : 300.f;
    ImGui::SetCursorPos({(W-cw)*0.5f, (H-ch)*0.5f});

    ImGui::PushStyleColor(ImGuiCol_ChildBg, to_u32(Col::bg_soft));
    ImGui::PushStyleColor(ImGuiCol_Border,  to_u32(Col::line));
    ImGui::BeginChild("##setup_card", {cw, ch}, ImGuiChildFlags_Borders);

    float iw = ImGui::GetContentRegionAvail().x;
    float pad = ImGui::GetStyle().WindowPadding.x + 16.f;

    ImGui::Dummy({0.f, 24.f});

    const char* title = "Lyric Extraction Setup";
    ImGui::SetCursorPosX((iw - ImGui::CalcTextSize(title).x) * 0.5f
                          + ImGui::GetStyle().WindowPadding.x);
    ImGui::PushFont(g_font_bold);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    ImGui::Dummy({0.f, 12.f});

    using S = AppState::SetupStage;
    S stage = state.setup_stage;

    if (stage == S::Idle) {
        ImGui::SetCursorPosX(pad);
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::SetNextItemWidth(iw - pad * 2.f);
        ImGui::TextWrapped(
            "Downloads faster-whisper large-v3 (~3 GB) and Demucs htdemucs (~80 MB) "
            "model weights into ~/.cache. Models are reused across runs.\n\n"
            "Requires the song2subs venv with whisperx and demucs installed.");
        ImGui::PopStyleColor();

        ImGui::Dummy({0.f, 20.f});

        float bw = 200.f, bh = 32.f;
        float bx = (iw - bw * 2.f - 12.f) * 0.5f + ImGui::GetStyle().WindowPadding.x;
        ImGui::SetCursorPosX(bx);

        ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(255,255,255,220));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(255,255,255,255));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(200,200,200,255));
        ImGui::PushStyleColor(ImGuiCol_Text,          IM_COL32(0,0,0,255));
        if (ImGui::Button("Download Models", {bw, bh})) {
            state.setup_running = true;
            state.setup_stage   = S::ModelDL;
            s_done = false; s_error = false;
            s_stage = S::ModelDL; s_progress = 0.f;
            s_worker_running = true;
            std::thread(model_dl_worker, g_prefetch_script).detach();
        }
        ImGui::PopStyleColor(4);

        ImGui::SameLine(0.f, 12.f);
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        if (ImGui::Button("Skip for now", {bw, bh}))
            state.models_skipped = true;
        ImGui::PopStyleColor();

    } else if (stage == S::ModelDL) {
        ImGui::SetCursorPosX((iw - ImGui::CalcTextSize("Downloading models…").x) * 0.5f
                              + ImGui::GetStyle().WindowPadding.x);
        ImGui::PushStyleColor(ImGuiCol_Text, Col::fg);
        ImGui::TextUnformatted("Downloading models…");
        ImGui::PopStyleColor();

        ImGui::Dummy({0.f, 16.f});
        draw_progress_bar(state.setup_progress, iw);

        if (!state.setup_message.empty()) {
            ImGui::SetCursorPosX(pad);
            ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
            std::string msg = state.setup_message;
            if (msg.size() > 80) msg = msg.substr(0, 77) + "…";
            ImGui::TextUnformatted(msg.c_str());
            ImGui::PopStyleColor();
        }

    } else if (stage == S::Error) {
        ImGui::SetCursorPosX(pad);
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255,80,80,255));
        ImGui::TextWrapped("Setup failed: %s", state.setup_error_msg.c_str());
        ImGui::PopStyleColor();

        ImGui::Dummy({0.f, 16.f});
        float bw = 140.f;
        ImGui::SetCursorPosX((iw - bw*2.f - 12.f)*0.5f + ImGui::GetStyle().WindowPadding.x);
        if (ui_btn("Try again", true, false)) {
            state.setup_running = true;
            state.setup_stage   = S::ModelDL;
            s_done = false; s_error = false;
            s_stage = S::ModelDL; s_progress = 0.f;
            s_worker_running = true;
            std::thread(model_dl_worker, g_prefetch_script).detach();
        }
        ImGui::SameLine(0.f, 12.f);
        if (ui_btn("Skip for now", false, false))
            state.models_skipped = true;

    } else if (stage == S::Done) {
        const char* msg = "Setup complete. You're all set.";
        ImGui::SetCursorPosX((iw - ImGui::CalcTextSize(msg).x) * 0.5f
                              + ImGui::GetStyle().WindowPadding.x);
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::TextUnformatted(msg);
        ImGui::PopStyleColor();

        ImGui::Dummy({0.f, 16.f});
        float bw = 160.f;
        ImGui::SetCursorPosX((iw - bw)*0.5f + ImGui::GetStyle().WindowPadding.x);
        if (ui_btn("Open Studio", true, false))
            state.models_skipped = true;
    }

    ImGui::EndChild();
    ImGui::PopStyleColor(2);
}

// ── Re-download modal (Help menu) ─────────────────────────────────────────────

static std::atomic<bool> s_dl_thread_running{false};
static std::mutex        s_dl_mutex;
static std::string       s_dl_stage;
static std::string       s_dl_message;
static float             s_dl_progress = 0.f;
static bool              s_dl_done     = false;
static bool              s_dl_error    = false;
static std::string       s_dl_error_msg;

static void model_only_worker(std::string script) {
    {
        std::lock_guard<std::mutex> lk(s_dl_mutex);
        s_dl_stage = "whisper"; s_dl_message = "Starting…";
        s_dl_progress = 0.f; s_dl_done = false;
        s_dl_error = false; s_dl_error_msg.clear();
    }
    std::string py = find_song2subs_python();
    if (py.empty()) {
        std::lock_guard<std::mutex> lk(s_dl_mutex);
        s_dl_error = true; s_dl_error_msg = "song2subs venv not found.";
        s_dl_thread_running = false; return;
    }
    std::string cmd = "\"" + py + "\" \"" + script + "\" 2>&1";
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) {
        std::lock_guard<std::mutex> lk(s_dl_mutex);
        s_dl_error = true; s_dl_error_msg = "Failed to launch prefetch script.";
        s_dl_thread_running = false; return;
    }
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        while (len>0&&(line[len-1]=='\n'||line[len-1]=='\r')) line[--len]=0;
        std::lock_guard<std::mutex> lk(s_dl_mutex);
        std::string l(line);
        if      (l.rfind("STAGE:",0)==0) { s_dl_stage=l.substr(6); s_dl_progress=(s_dl_stage=="demucs")?0.5f:0.f; }
        else if (l.rfind("OK:",   0)==0) { s_dl_progress=(l.substr(3)=="whisper")?0.5f:1.f; }
        else if (l.rfind("ERROR:",0)==0) { auto p=l.find(':',6); s_dl_error=true; s_dl_error_msg=(p!=std::string::npos)?l.substr(p+1):l.substr(6); }
        else if (l=="DONE")              { s_dl_done=true; s_dl_progress=1.f; }
        else                             { s_dl_message=l; }
    }
    pclose(fp);
    s_dl_thread_running = false;
}

void ui_model_download_modal(AppState& state) {
    if (!state.show_model_dl_modal) return;

    ImGui::OpenPopup("##model_dl_modal");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({460.f, 0.f});
    ImGui::PushStyleColor(ImGuiCol_PopupBg, to_u32(Col::bg));
    ImGui::PushStyleColor(ImGuiCol_Border,  to_u32(Col::line));

    if (ImGui::BeginPopupModal("##model_dl_modal", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize)) {

        if (state.model_dl_running) {
            std::lock_guard<std::mutex> lk(s_dl_mutex);
            state.model_dl_stage    = s_dl_stage;
            state.model_dl_message  = s_dl_message;
            state.model_dl_progress = s_dl_progress;
            state.model_dl_error    = s_dl_error;
            state.model_dl_error_msg= s_dl_error_msg;
            if (s_dl_done)  { state.model_dl_done=true; state.model_dl_running=false; state.models_ready=true; }
            if (s_dl_error && !s_dl_thread_running) state.model_dl_running=false;
        }

        float iw = ImGui::GetContentRegionAvail().x;
        float pad = ImGui::GetStyle().WindowPadding.x + 8.f;

        ImGui::Dummy({0.f, 12.f});
        ImGui::SetCursorPosX(pad);
        ImGui::PushFont(g_font_bold);
        ImGui::TextUnformatted("Download Lyric Extraction Models");
        ImGui::PopFont();
        ImGui::Dummy({0.f, 8.f});

        if (!state.model_dl_running && !state.model_dl_done) {
            ImGui::SetCursorPosX(pad);
            ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
            if (state.model_dl_error)
                ImGui::TextWrapped("Previous attempt failed: %s\n\nClick Download to retry.",
                    state.model_dl_error_msg.c_str());
            else
                ImGui::TextWrapped("Downloads faster-whisper large-v3 and Demucs htdemucs (~3.1 GB). "
                    "Stored permanently in ~/.cache.");
            ImGui::PopStyleColor();
            ImGui::Dummy({0.f, 12.f});
            if (ui_btn("Download", true, false)) {
                state.model_dl_running=true; state.model_dl_error=false;
                s_dl_done=false; s_dl_error=false; s_dl_thread_running=true;
                std::thread(model_only_worker, g_prefetch_script).detach();
            }
            ImGui::SameLine(0.f, 8.f);
            if (ui_btn("Cancel", false, false)) {
                state.show_model_dl_modal=false; ImGui::CloseCurrentPopup();
            }
        } else if (state.model_dl_running) {
            const char* lbl = (state.model_dl_stage=="demucs")
                ? "Downloading Demucs htdemucs…"
                : "Downloading faster-whisper large-v3…";
            ImGui::SetCursorPosX(pad);
            ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
            ImGui::TextUnformatted(lbl);
            ImGui::PopStyleColor();
            ImGui::Dummy({0.f, 8.f});
            draw_progress_bar(state.model_dl_progress, iw);
        } else if (state.model_dl_done) {
            ImGui::SetCursorPosX(pad);
            ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
            ImGui::TextUnformatted("Models installed successfully.");
            ImGui::PopStyleColor();
            ImGui::Dummy({0.f, 12.f});
            if (ui_btn("Close", false, false)) {
                state.show_model_dl_modal=false; ImGui::CloseCurrentPopup();
            }
        }

        ImGui::Dummy({0.f, 8.f});
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor(2);
}
