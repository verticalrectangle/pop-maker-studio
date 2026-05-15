#include "vc_job.h"
#include "vc_onnx.h"
#include <filesystem>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

namespace fs = std::filesystem;

// ── Path helpers ──────────────────────────────────────────────────────────────

static std::string managed_dir() {
    const char* h = getenv("HOME");
    return h ? (std::string(h) + "/.local/share/pop-maker-studio") : "/tmp/pop-maker-studio";
}

static std::string vc_venv_dir() { return managed_dir() + "/vc_venv"; }
static std::string vc_venv_py()  { return vc_venv_dir() + "/bin/python3"; }

// ── Script discovery ──────────────────────────────────────────────────────────

// The script source lives at tools/vc_infer.py in the repo.
// At runtime we find it relative to the binary, or fall back to the managed copy.
static std::string find_tool_script(const std::string& name) {
    fs::path self;
    char buf[4096] = {};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf)-1);
    if (n > 0) self = fs::path(buf).parent_path();

    for (const auto& base : {
        self / "tools",
        self / "../tools",
        self / "../../tools",
    }) {
        std::error_code ec;
        fs::path c = base / name;
        if (fs::exists(c, ec)) return c.string();
    }
    return "";
}

// ── System python3 detection ──────────────────────────────────────────────────

static std::string find_system_python() {
    for (const char* name : {"python3", "python"}) {
        std::string cmd = std::string("command -v ") + name + " 2>/dev/null";
        FILE* fp = popen(cmd.c_str(), "r");
        if (!fp) continue;
        char path[512] = {};
        fgets(path, sizeof(path), fp);
        pclose(fp);
        // strip newline
        size_t len = strlen(path);
        while (len && (path[len-1]=='\n'||path[len-1]=='\r')) path[--len]=0;
        if (!len) continue;
        // confirm it's Python 3.x
        std::string ver_cmd = std::string("\"") + path + "\" --version 2>&1";
        FILE* vp = popen(ver_cmd.c_str(), "r");
        if (!vp) continue;
        char ver[64] = {};
        fgets(ver, sizeof(ver), vp);
        pclose(vp);
        if (strstr(ver, "Python 3.")) return path;
    }
    return "";
}

// ── Venv bootstrap ────────────────────────────────────────────────────────────

// Runs in its own thread. Reports progress via progress_cb(0..1, message).
// Returns error string or empty on success.
static std::string bootstrap_venv(
    const std::string& syspy,
    std::function<void(float, const std::string&)> on_progress)
{
    std::string venv = vc_venv_dir();
    std::string py   = vc_venv_py();

    // Already bootstrapped?
    if (fs::exists(py)) return "";

    on_progress(0.02f, "Creating Python environment…");
    std::string cmd = "\"" + syspy + "\" -m venv \"" + venv + "\" 2>&1";
    if (system(cmd.c_str()) != 0)   // NOLINT
        return "Failed to create venv (python3 -m venv)";

    on_progress(0.05f, "Upgrading pip…");
    cmd = "\"" + py + "\" -m pip install --quiet --upgrade pip 2>&1";
    system(cmd.c_str());  // NOLINT — best-effort

    // Install torch CPU-only + torchaudio + numpy
    static const struct { const char* pkg; float frac; const char* label; } kPkgs[] = {
        { "torch --index-url https://download.pytorch.org/whl/cpu",
          0.65f, "Downloading PyTorch (CPU)… this takes a few minutes" },
        { "torchaudio --index-url https://download.pytorch.org/whl/cpu",
          0.85f, "Downloading torchaudio…" },
        { "numpy",
          0.98f, "Installing numpy…" },
    };
    for (auto& p : kPkgs) {
        on_progress(p.frac - 0.02f, p.label);
        cmd = "\"" + py + "\" -m pip install --quiet " + p.pkg + " 2>&1";
        if (system(cmd.c_str()) != 0)  // NOLINT
            return std::string("pip install failed: ") + p.pkg;
        on_progress(p.frac, p.label);
    }
    return "";
}

// ── Per-job state ─────────────────────────────────────────────────────────────

struct VcJobData {
    std::atomic<float>  progress{0.f};
    std::atomic<int>    status{0};   // 0=running 1=done 2=error
    std::string         out_path;
    std::string         error_msg;
    std::string         phase;       // "bootstrap" | "converting"
    std::mutex          mu;
};

struct VcJob {
    std::shared_ptr<VcJobData> data;
    int    track_idx = -1;
    int    clip_idx  = -1;
    std::thread thread;
};

static std::vector<VcJob> g_jobs;
static std::mutex          g_jobs_mu;

// ── Background worker ─────────────────────────────────────────────────────────

static void run_job(std::shared_ptr<VcJobData> data,
                    const std::string& source_path,
                    const std::string& model_path,
                    const std::string& out_path,
                    int f0_semitones)
{
    auto set_prog = [&](float p, const std::string& ph) {
        data->progress.store(p);
        std::lock_guard<std::mutex> lk(data->mu);
        data->phase = ph;
    };
    auto set_err = [&](const std::string& msg) {
        std::lock_guard<std::mutex> lk(data->mu);
        data->error_msg = msg;
        data->status.store(2);
    };

    // ── Determine ONNX paths ──────────────────────────────────────────────────
    // Voice ONNX lives alongside the .pth, with .onnx extension.
    std::string voice_onnx = model_path.substr(0, model_path.rfind('.')) + ".onnx";

    bool use_onnx = fs::exists(voice_onnx) && hubert_onnx_exists();

    // ── One-time ONNX export via Python ──────────────────────────────────────
    if (!use_onnx) {
        std::string infer_script  = find_tool_script("vc_infer.py");
        std::string export_script = find_tool_script("vc_export.py");

        if (export_script.empty() || infer_script.empty()) {
            set_err("vc_infer.py / vc_export.py not found — check tools/ directory");
            return;
        }

        std::string syspy = find_system_python();
        if (syspy.empty()) {
            set_err("Python 3 not found. Install from python.org then try again.");
            return;
        }

        if (!fs::exists(vc_venv_py())) {
            set_prog(0.01f, "bootstrap");
            std::string err = bootstrap_venv(syspy, [&](float p, const std::string& msg) {
                set_prog(p * 0.25f, "bootstrap:" + msg);
            });
            if (!err.empty()) { set_err(err); return; }
        }

        std::string py = vc_venv_py();

        // Ensure hubert ONNX output directory exists
        fs::path hub_path = hubert_onnx_path();
        std::error_code ec;
        fs::create_directories(hub_path.parent_path(), ec);

        // Run vc_export.py — converts .pth → .onnx + exports hubert.onnx
        set_prog(0.26f, "exporting");
        std::string exp_cmd = "\"" + py + "\" \"" + export_script + "\""
                            + " \"" + model_path  + "\""
                            + " \"" + voice_onnx  + "\""
                            + " \"" + hub_path.string() + "\""
                            + " 2>&1";

        FILE* efp = popen(exp_cmd.c_str(), "r");
        if (!efp) { set_err("Failed to launch vc_export.py"); return; }

        std::string last_exp_err;
        char eline[512];
        while (fgets(eline, sizeof(eline), efp)) {
            float p = 0.f;
            if (sscanf(eline, "PROGRESS:%f", &p) == 1)
                set_prog(0.26f + p * 0.34f, "exporting model");
            else if (strncmp(eline, "ERROR", 5) == 0)
                last_exp_err = std::string(eline).substr(6);
        }
        int exp_rc = pclose(efp);

        use_onnx = (exp_rc == 0) && fs::exists(voice_onnx) && hubert_onnx_exists();

        if (!use_onnx) {
            // Export failed — fall back to full Python inference via vc_infer.py
            if (last_exp_err.empty())
                last_exp_err = "ONNX export failed (rc=" + std::to_string(exp_rc) + ")";
            // Log but continue — fall through to Python mode below
            set_prog(0.35f, "python-fallback:" + last_exp_err);
        }
    }

    // ── Decode source audio ───────────────────────────────────────────────────
    set_prog(use_onnx ? 0.60f : 0.35f, "converting");
    std::string wav_in = out_path + ".src.wav";
    std::string dec_cmd = "ffmpeg -hide_banner -loglevel error -y"
                          " -i \"" + source_path + "\""
                          " -vn -ar 44100 -ac 1 \"" + wav_in + "\" 2>/dev/null";
    if (system(dec_cmd.c_str()) != 0 || !fs::exists(wav_in)) {  // NOLINT
        set_err("Failed to decode source audio");
        return;
    }

    // ── Inference ─────────────────────────────────────────────────────────────
    if (use_onnx) {
        // ── C++ ONNX path (fast, no Python at runtime) ────────────────────────
        std::string err = vc_onnx_convert(wav_in, voice_onnx, out_path, f0_semitones,
            [&](float p, const std::string& msg) {
                set_prog(0.62f + p * 0.38f, "converting:" + msg);
            });
        fs::remove(wav_in);
        if (!err.empty()) { set_err(err); return; }
    } else {
        // ── Python fallback path ──────────────────────────────────────────────
        std::string infer_script = find_tool_script("vc_infer.py");
        std::string py = vc_venv_py();
        if (infer_script.empty() || py.empty()) {
            fs::remove(wav_in);
            set_err("Voice conversion failed: no ONNX models and no Python fallback");
            return;
        }

        std::string cmd = "\"" + py + "\" \"" + infer_script + "\""
                        + " \"" + wav_in     + "\""
                        + " \"" + model_path + "\""
                        + " \"" + out_path   + "\""
                        + " " + std::to_string(f0_semitones)
                        + " 2>&1";

        FILE* fp = popen(cmd.c_str(), "r");
        if (!fp) { fs::remove(wav_in); set_err("Failed to launch vc_infer.py"); return; }

        std::string last_err;
        char line[512];
        while (fgets(line, sizeof(line), fp)) {
            float p = 0.f;
            if (sscanf(line, "PROGRESS:%f", &p) == 1)
                set_prog(0.40f + p * 0.60f, "converting");
            else if (strncmp(line, "ERROR", 5) == 0 || strncmp(line, "Traceback", 9) == 0) {
                last_err = std::string(line);
                last_err.erase(last_err.find_last_not_of("\r\n") + 1);
            }
        }
        int rc = pclose(fp);
        fs::remove(wav_in);

        if (rc != 0 || !fs::exists(out_path)) {
            set_err(last_err.empty() ? "Voice conversion failed" : last_err);
            return;
        }
    }

    data->progress.store(1.f);
    {
        std::lock_guard<std::mutex> lk(data->mu);
        data->out_path = out_path;
        data->phase    = "done";
    }
    data->status.store(1);
}

// ── Public API ────────────────────────────────────────────────────────────────

void vc_start(AppState& state, int track_idx, int clip_idx,
              const std::string& model_path, int f0_semitones)
{
    if (track_idx < 0 || track_idx >= (int)state.tracks.size()) return;
    auto& track = state.tracks[track_idx];
    if (clip_idx < 0 || clip_idx >= (int)track.clips.size()) return;
    Clip& clip = track.clips[clip_idx];
    if (clip.clip_type != ClipType::Audio || clip.text.empty()) return;

    uint64_t h = std::hash<std::string>{}(clip.text + model_path + std::to_string(f0_semitones));
    std::string out_path = "/tmp/pms_vc_" + std::to_string(h) + ".wav";

    clip.vc_status    = VcStatus::Processing;
    clip.vc_progress  = 0.f;
    clip.vc_out_path.clear();
    clip.vc_model_used = model_path;
    clip.vc_error.clear();

    auto data     = std::make_shared<VcJobData>();
    data->out_path = out_path;

    VcJob job;
    job.data      = data;
    job.track_idx = track_idx;
    job.clip_idx  = clip_idx;
    job.thread    = std::thread(run_job, data, clip.text, model_path, out_path, f0_semitones);

    std::lock_guard<std::mutex> lk(g_jobs_mu);
    g_jobs.push_back(std::move(job));
}

void vc_poll(AppState& state)
{
    std::lock_guard<std::mutex> lk(g_jobs_mu);
    for (auto it = g_jobs.begin(); it != g_jobs.end(); ) {
        auto& job = *it;
        int   st  = job.data->status.load();

        auto get_clip = [&]() -> Clip* {
            if (job.track_idx < 0 || job.track_idx >= (int)state.tracks.size()) return nullptr;
            auto& t = state.tracks[job.track_idx];
            if (job.clip_idx < 0 || job.clip_idx >= (int)t.clips.size()) return nullptr;
            return &t.clips[job.clip_idx];
        };

        Clip* clip = get_clip();
        if (!clip) {
            if (st != 0 && job.thread.joinable()) job.thread.join();
            it = (st != 0) ? g_jobs.erase(it) : ++it;
            continue;
        }

        if (st == 0) {
            clip->vc_progress = job.data->progress.load();
            ++it;
        } else if (st == 1) {
            clip->vc_progress = 1.f;
            clip->vc_out_path = job.data->out_path;
            clip->vc_status   = VcStatus::Ready;
            if (job.thread.joinable()) job.thread.join();
            it = g_jobs.erase(it);
        } else {
            {
                std::lock_guard<std::mutex> lk2(job.data->mu);
                clip->vc_error = job.data->error_msg;
            }
            clip->vc_status = VcStatus::Error;
            if (job.thread.joinable()) job.thread.join();
            it = g_jobs.erase(it);
        }
    }
}

void vc_cancel_all()
{
    std::lock_guard<std::mutex> lk(g_jobs_mu);
    for (auto& job : g_jobs)
        if (job.thread.joinable()) job.thread.detach();
    g_jobs.clear();
}

// Returns true if the ML venv is already installed.
bool vc_venv_ready() {
    return fs::exists(vc_venv_py());
}

// Returns the venv python3 path, or empty string if not installed.
std::string vc_venv_python_path() {
    std::string p = vc_venv_py();
    return fs::exists(p) ? p : "";
}
