#include "screens.h"
#include "theme.h"
#include "../app.h"
#include <imgui.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <atomic>
#include <string>
#include <mutex>
#include <functional>
#include <filesystem>
#include <unistd.h>

extern ImFont* g_font_bold;

namespace fs = std::filesystem;

static constexpr const char* k_model_url =
    "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-large-v3-turbo-q5_0.bin";
static constexpr int64_t k_model_size_approx = 584LL * 1024 * 1024;

static std::string ggml_dest_path() {
    const char* home = getenv("HOME");
    if (!home) return "/tmp/ggml-large-v3-turbo-q5_0.bin";
    return std::string(home) + "/.cache/pop-maker-studio/whisper/ggml-large-v3-turbo-q5_0.bin";
}

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

// ── Progress bar ──────────────────────────────────────────────────────────────

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

// ── Core download function ────────────────────────────────────────────────────

// Downloads the ggml whisper model via curl with file-size polling for progress.
// Calls set_progress(frac, message) on each tick, set_done() on success,
// set_error(msg) on failure. Blocking — run in a thread.
static void download_ggml(
    std::function<void(float, const std::string&)> set_progress,
    std::function<void()>                          set_done,
    std::function<void(const std::string&)>        set_error)
{
    std::string dest = ggml_dest_path();

    // Already complete?
    if (fs::exists(dest) && (int64_t)fs::file_size(dest) >= k_model_size_approx - 10*1024*1024) {
        set_done();
        return;
    }

    fs::create_directories(fs::path(dest).parent_path());

    set_progress(0.f, "Querying model size…");

    // Probe actual file size via HEAD (follow redirects)
    int64_t total = k_model_size_approx;
    {
        std::string cmd = std::string("curl -sIL \"") + k_model_url +
                          "\" 2>/dev/null | grep -i 'content-length' | tail -1";
        FILE* fp = popen(cmd.c_str(), "r");
        if (fp) {
            char buf[128] = "";
            fgets(buf, sizeof(buf), fp);
            pclose(fp);
            const char* p = strrchr(buf, ' ');
            if (p) { int64_t n = atoll(p + 1); if (n > 10*1024*1024) total = n; }
        }
    }

    set_progress(0.f, "Starting download…");

    // Sentinel file: when curl finishes it appends "done" to this file.
    std::string sentinel = std::string("/tmp/pms_dl_") + std::to_string((long)getpid()) + ".done";
    std::string errfile  = std::string("/tmp/pms_dl_") + std::to_string((long)getpid()) + ".err";
    fs::remove(sentinel);
    fs::remove(errfile);

    // Launch curl in the background via shell; -C - resumes partial downloads.
    std::string cmd =
        "curl -fsSL -C - -o \"" + dest + "\" \"" + k_model_url + "\""
        " 2>\"" + errfile + "\""
        " && echo done > \"" + sentinel + "\""
        " || echo fail > \"" + sentinel + "\"";
    // Run in background shell subprocess
    std::thread curl_thread([cmd]{ system(cmd.c_str()); }); // NOLINT
    curl_thread.detach();

    // Poll file size until sentinel appears
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));

        int64_t current = 0;
        if (fs::exists(dest)) {
            std::error_code ec;
            current = (int64_t)fs::file_size(dest, ec);
        }

        float frac = (total > 0) ? (float)((double)current / total) : 0.f;
        frac = fmaxf(0.f, fminf(frac, 0.99f));

        std::string label = "Downloading ggml-large-v3-turbo…  "
            + std::to_string(current / (1024*1024)) + " MB / "
            + std::to_string(total   / (1024*1024)) + " MB";
        set_progress(frac, label);

        // Check sentinel
        if (fs::exists(sentinel)) {
            char result[8] = "";
            FILE* sf = fopen(sentinel.c_str(), "r");
            if (sf) { fgets(result, sizeof(result), sf); fclose(sf); }
            fs::remove(sentinel);
            fs::remove(errfile);

            if (strncmp(result, "done", 4) == 0) {
                set_done();
            } else {
                set_error("curl download failed. Check your internet connection and try again.");
            }
            return;
        }
    }
}

// ── Setup-screen worker ───────────────────────────────────────────────────────

static void model_dl_worker() {
    using S = AppState::SetupStage;
    worker_set(S::ModelDL, 0.f, "Preparing…");

    download_ggml(
        [](float p, const std::string& msg) { worker_set(AppState::SetupStage::ModelDL, p, msg); },
        []() {
            std::lock_guard<std::mutex> lk(s_worker_mutex);
            s_done = true; s_progress = 1.f;
            s_stage = AppState::SetupStage::Done;
            s_message = "Model downloaded successfully.";
            s_worker_running = false;
        },
        [](const std::string& err) { worker_err(err); }
    );
    s_worker_running = false;
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
            "Downloads the Whisper ggml-large-v3-turbo model (~584 MB) into ~/.cache. "
            "The model is reused across runs and enables word-level lyric extraction.");
        ImGui::PopStyleColor();

        ImGui::Dummy({0.f, 20.f});

        float bw = 200.f, bh = 32.f;
        float bx = (iw - bw * 2.f - 12.f) * 0.5f + ImGui::GetStyle().WindowPadding.x;
        ImGui::SetCursorPosX(bx);

        ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(255,255,255,220));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(255,255,255,255));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(200,200,200,255));
        ImGui::PushStyleColor(ImGuiCol_Text,          IM_COL32(0,0,0,255));
        if (ImGui::Button("Download Model", {bw, bh})) {
            state.setup_running = true;
            state.setup_stage   = S::ModelDL;
            s_done = false; s_error = false;
            s_stage = S::ModelDL; s_progress = 0.f;
            s_worker_running = true;
            std::thread(model_dl_worker).detach();
        }
        ImGui::PopStyleColor(4);

        ImGui::SameLine(0.f, 12.f);
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        if (ImGui::Button("Skip for now", {bw, bh}))
            state.models_skipped = true;
        ImGui::PopStyleColor();

    } else if (stage == S::ModelDL) {
        ImGui::SetCursorPosX((iw - ImGui::CalcTextSize("Downloading model…").x) * 0.5f
                              + ImGui::GetStyle().WindowPadding.x);
        ImGui::PushStyleColor(ImGuiCol_Text, Col::fg);
        ImGui::TextUnformatted("Downloading model…");
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
            std::thread(model_dl_worker).detach();
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
static std::string       s_dl_message;
static float             s_dl_progress = 0.f;
static bool              s_dl_done     = false;
static bool              s_dl_error    = false;
static std::string       s_dl_error_msg;

static void model_only_worker() {
    {
        std::lock_guard<std::mutex> lk(s_dl_mutex);
        s_dl_message = "Preparing…";
        s_dl_progress = 0.f; s_dl_done = false;
        s_dl_error = false; s_dl_error_msg.clear();
    }

    download_ggml(
        [](float p, const std::string& msg) {
            std::lock_guard<std::mutex> lk(s_dl_mutex);
            s_dl_progress = p; s_dl_message = msg;
        },
        []() {
            std::lock_guard<std::mutex> lk(s_dl_mutex);
            s_dl_done = true; s_dl_progress = 1.f;
            s_dl_thread_running = false;
        },
        [](const std::string& err) {
            std::lock_guard<std::mutex> lk(s_dl_mutex);
            s_dl_error = true; s_dl_error_msg = err;
            s_dl_thread_running = false;
        }
    );
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
        ImGui::TextUnformatted("Download Whisper Model");
        ImGui::PopFont();
        ImGui::Dummy({0.f, 8.f});

        if (!state.model_dl_running && !state.model_dl_done) {
            ImGui::SetCursorPosX(pad);
            ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
            if (state.model_dl_error)
                ImGui::TextWrapped("Previous attempt failed: %s\n\nClick Download to retry.",
                    state.model_dl_error_msg.c_str());
            else
                ImGui::TextWrapped("Downloads ggml-large-v3-turbo-q5_0 (~584 MB). "
                    "Stored permanently in ~/.cache/pop-maker-studio/whisper/.");
            ImGui::PopStyleColor();
            ImGui::Dummy({0.f, 12.f});
            if (ui_btn("Download", true, false)) {
                state.model_dl_running=true; state.model_dl_error=false;
                s_dl_done=false; s_dl_error=false; s_dl_thread_running=true;
                std::thread(model_only_worker).detach();
            }
            ImGui::SameLine(0.f, 8.f);
            if (ui_btn("Cancel", false, false)) {
                state.show_model_dl_modal=false; ImGui::CloseCurrentPopup();
            }
        } else if (state.model_dl_running) {
            ImGui::SetCursorPosX(pad);
            ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
            std::string msg = state.model_dl_message;
            if (msg.size() > 70) msg = msg.substr(0, 67) + "…";
            ImGui::TextUnformatted(msg.empty() ? "Downloading…" : msg.c_str());
            ImGui::PopStyleColor();
            ImGui::Dummy({0.f, 8.f});
            draw_progress_bar(state.model_dl_progress, iw);
        } else if (state.model_dl_done) {
            ImGui::SetCursorPosX(pad);
            ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
            ImGui::TextUnformatted("Model installed successfully.");
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
