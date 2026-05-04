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

// Run a shell command via popen, invoke callback for each output line.
// Returns false if the command failed (non-zero exit or popen error).
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

// ── Full setup worker (Python → pip → models) ─────────────────────────────────

static void full_setup_worker(std::string setup_script, std::string prefetch_script) {
    fs::path managed   = fs::path(g_managed_dir);
    fs::path tarball   = fs::temp_directory_path() / "pop_maker_python311.tar.gz";
    fs::path python_dir = managed / "python";
    fs::path python_bin = python_dir / "bin" / "python3";
    fs::path venv_dir   = managed / "venv";
    fs::path venv_py    = venv_dir / "bin" / "python3";

    // ── Step 1: Extract embedded Python runtime ───────────────────────────────
    if (!fs::exists(python_bin)) {
        worker_set(AppState::SetupStage::PythonExtract, 0.f, "Extracting Python 3.11…");
        std::error_code ec;
        fs::create_directories(managed, ec);

        // Pipe the embedded tarball bytes directly into tar's stdin — no temp file.
        std::string tar_cmd = "tar -xzf - -C \"" + managed.string() + "\" 2>&1";
        FILE* tar_proc = popen(tar_cmd.c_str(), "w");
        if (!tar_proc) return worker_err("Failed to launch tar.");

        const unsigned char* data  = python311_tar_data();
        size_t               total = python311_tar_size();
        size_t written = fwrite(data, 1, total, tar_proc);
        int rc = pclose(tar_proc);

        if (rc != 0 || written != total || !fs::exists(python_bin))
            return worker_err("Failed to extract Python runtime.");

        worker_set(AppState::SetupStage::PythonExtract, 0.1f, "Python 3.11 ready.");
    }

    // ── Step 3: Create venv ───────────────────────────────────────────────────
    if (!fs::exists(venv_py)) {
        worker_set(AppState::SetupStage::PipInstall, 0.15f, "Creating Python environment…");
        std::string venv_cmd = "\"" + python_bin.string()
                             + "\" -m venv \"" + venv_dir.string() + "\" 2>&1";
        run_cmd(venv_cmd, [](const std::string&){});
        if (!fs::exists(venv_py)) {
            return worker_err("Failed to create Python environment.");
        }
    }

    // ── Step 4: pip install via ml_setup.py (handles CUDA detection) ─────────
    bool pkgs_ok = false;
    {
        std::string chk = "\"" + venv_py.string()
                        + "\" -c \"import whisperx, demucs\" 2>&1";
        FILE* fp = popen(chk.c_str(), "r");
        if (fp) { pkgs_ok = (pclose(fp) == 0); }
    }

    if (!pkgs_ok) {
        worker_set(AppState::SetupStage::PipInstall, 0.20f, "Installing packages…");
        std::string setup_cmd = "\"" + venv_py.string()
                              + "\" \"" + setup_script + "\" 2>&1";
        run_cmd(setup_cmd, [](const std::string& line){
            std::lock_guard<std::mutex> lk(s_worker_mutex);
            if (line.rfind("STAGE:", 0) == 0) {
                std::string stg = line.substr(6);
                if      (stg == "torch")    { s_progress = 0.20f; s_message = "Installing torch…"; }
                else if (stg == "whisperx") { s_progress = 0.32f; s_message = "Installing whisperx…"; }
                else if (stg == "demucs")   { s_progress = 0.40f; s_message = "Installing demucs…"; }
            } else if (line.rfind("OK:", 0) == 0) {
                std::string stg = line.substr(3);
                if      (stg == "torch")    s_progress = 0.32f;
                else if (stg == "whisperx") s_progress = 0.40f;
                else if (stg == "demucs")   s_progress = 0.48f;
            } else if (line.rfind("ERROR:", 0) == 0) {
                auto sep = line.find(':', 6);
                s_error     = true;
                s_error_msg = (sep != std::string::npos) ? line.substr(sep+1) : line.substr(6);
            } else if (line == "DONE") {
                s_progress = 0.48f;
            } else {
                s_message = line;
            }
        });
        if (s_error) { s_worker_running = false; return; }
    }

    // ── Step 5: Download model weights ────────────────────────────────────────
    worker_set(AppState::SetupStage::ModelDL, 0.5f, "Starting model download…");

    std::string prefetch_cmd = "\"" + venv_py.string()
                             + "\" \"" + prefetch_script + "\" 2>&1";
    run_cmd(prefetch_cmd, [](const std::string& line){
        std::lock_guard<std::mutex> lk(s_worker_mutex);
        if (line.rfind("STAGE:", 0) == 0) {
            std::string stg = line.substr(6);
            s_progress = (stg == "demucs") ? 0.75f : 0.5f;
            s_stage = AppState::SetupStage::ModelDL;
            s_message = (stg == "demucs") ? "Downloading Demucs htdemucs…"
                                           : "Downloading faster-whisper large-v3…";
        } else if (line.rfind("OK:", 0) == 0) {
            s_progress = (line.substr(3) == "whisper") ? 0.75f : 1.f;
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
        // prefetch exited without DONE — treat as error
        std::lock_guard<std::mutex> lk(s_worker_mutex);
        s_error     = true;
        s_error_msg = "Model download did not complete.";
    }
    s_worker_running = false;
}

// ── Model-only worker (re-download from Help menu) ────────────────────────────

static std::atomic<bool> s_dl_thread_running{false};
static std::mutex        s_dl_mutex;
static std::string       s_dl_stage;
static std::string       s_dl_message;
static float             s_dl_progress = 0.f;
static bool              s_dl_done     = false;
static bool              s_dl_error    = false;
static std::string       s_dl_error_msg;

static void model_only_worker(std::string python, std::string script) {
    {
        std::lock_guard<std::mutex> lk(s_dl_mutex);
        s_dl_stage = "whisper"; s_dl_message = "Starting…";
        s_dl_progress = 0.f; s_dl_done = false;
        s_dl_error = false; s_dl_error_msg.clear();
    }
    std::string cmd = "\"" + python + "\" \"" + script + "\" 2>&1";
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
    // Sync worker state each frame
    if (state.setup_running) {
        std::lock_guard<std::mutex> lk(s_worker_mutex);
        state.setup_stage    = s_stage;
        state.setup_progress = s_progress;
        state.setup_message  = s_message;
        if (s_error) {
            state.setup_stage    = AppState::SetupStage::Error;
            state.setup_error_msg = s_error_msg;
            state.setup_running  = false;
        } else if (s_done) {
            state.setup_stage   = AppState::SetupStage::Done;
            state.setup_running = false;
            state.models_ready  = true;
            // Point python_path at the managed venv
            if (!g_managed_dir.empty())
                state.python_path = g_managed_dir + "/venv/bin/python3";
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
    const float ch = is_running ? 280.f : 340.f;
    ImGui::SetCursorPos({(W-cw)*0.5f, (H-ch)*0.5f});

    ImGui::PushStyleColor(ImGuiCol_ChildBg, to_u32(Col::bg_soft));
    ImGui::PushStyleColor(ImGuiCol_Border,  to_u32(Col::line));
    ImGui::BeginChild("##setup_card", {cw, ch}, ImGuiChildFlags_Borders);

    float iw = ImGui::GetContentRegionAvail().x;
    float pad = ImGui::GetStyle().WindowPadding.x + 16.f;

    ImGui::Dummy({0.f, 24.f});

    // Title
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
        // ── Prompt ────────────────────────────────────────────────────────────
        ImGui::SetCursorPosX(pad);
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::SetNextItemWidth(iw - pad * 2.f);
        ImGui::TextWrapped(
            "Pop Maker Studio can separate vocals and transcribe lyrics "
            "using on-device AI.\n\n"
            "First-time setup downloads:\n"
            "  • whisperx + demucs packages  (~500 MB)\n"
            "  • faster-whisper large-v3 model  (~3 GB)\n"
            "  • Demucs htdemucs model  (~80 MB)\n\n"
            "Python 3.11 is built in. Everything else stays on your "
            "machine permanently.");
        ImGui::PopStyleColor();

        ImGui::Dummy({0.f, 20.f});

        float bw = 200.f, bh = 32.f;
        float bx = (iw - bw * 2.f - 12.f) * 0.5f + ImGui::GetStyle().WindowPadding.x;
        ImGui::SetCursorPosX(bx);

        ImGui::PushStyleColor(ImGuiCol_Button,       IM_COL32(255,255,255,220));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(255,255,255,255));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(200,200,200,255));
        ImGui::PushStyleColor(ImGuiCol_Text,          IM_COL32(0,0,0,255));
        if (ImGui::Button("Set Up Now", {bw, bh})) {
            state.setup_running = true;
            state.setup_stage   = S::PythonExtract;
            s_done = false; s_error = false;
            s_stage = S::PythonExtract; s_progress = 0.f;
            s_worker_running = true;
            std::thread(full_setup_worker, g_setup_script, g_prefetch_script).detach();
        }
        ImGui::PopStyleColor(4);

        ImGui::SameLine(0.f, 12.f);
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        if (ImGui::Button("Skip for now", {bw, bh}))
            state.models_skipped = true;
        ImGui::PopStyleColor();

    } else if (stage == S::PythonExtract || stage == S::PipInstall || stage == S::ModelDL) {
        // ── Progress ──────────────────────────────────────────────────────────
        const char* stage_label =
            stage == S::PythonExtract ? "Step 1 of 3  —  Setting up Python 3.11" :
            stage == S::PipInstall    ? "Step 2 of 3  —  Installing packages" :
                                        "Step 3 of 3  —  Downloading models";

        ImGui::SetCursorPosX((iw - ImGui::CalcTextSize(stage_label).x) * 0.5f
                              + ImGui::GetStyle().WindowPadding.x);
        ImGui::PushStyleColor(ImGuiCol_Text, Col::fg);
        ImGui::TextUnformatted(stage_label);
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
        // ── Error ─────────────────────────────────────────────────────────────
        ImGui::SetCursorPosX(pad);
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255,80,80,255));
        ImGui::TextWrapped("Setup failed: %s", state.setup_error_msg.c_str());
        ImGui::PopStyleColor();

        ImGui::Dummy({0.f, 16.f});
        float bw = 140.f;
        ImGui::SetCursorPosX((iw - bw*2.f - 12.f)*0.5f + ImGui::GetStyle().WindowPadding.x);
        if (ui_btn("Try again", true, false)) {
            state.setup_running = true;
            state.setup_stage   = S::PythonExtract;
            s_done = false; s_error = false;
            s_stage = S::PythonExtract; s_progress = 0.f;
            s_worker_running = true;
            std::thread(full_setup_worker, g_setup_script, g_prefetch_script).detach();
        }
        ImGui::SameLine(0.f, 12.f);
        if (ui_btn("Skip for now", false, false))
            state.models_skipped = true;

    } else if (stage == S::Done) {
        // ── Success ───────────────────────────────────────────────────────────
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

// ── Re-download modal (Help menu — assumes Python + packages already present) ─

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
                std::thread(model_only_worker, state.python_path, g_prefetch_script).detach();
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
