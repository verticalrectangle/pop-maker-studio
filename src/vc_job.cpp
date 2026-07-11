#include "vc_job.h"
#include "platform.h"
#include "vc_onnx.h"
#include "rmvpe_onnx.h"
#include "rvc_onnx.h"
#include "pth_reader.h"
#include <filesystem>
#include <fstream>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ── Per-job state ─────────────────────────────────────────────────────────────

struct VcJobData {
    std::atomic<float>  progress{0.f};
    std::atomic<int>    status{0};   // 0=running 1=done 2=error
    std::string         out_path;
    std::string         error_msg;
    std::string         phase;
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
                    int f0_semitones, bool f0_auto)
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

    // ── Model availability checks (fail fast, before the slow export) ────────
    if (!hubert_onnx_exists()) {
        set_err("HuBERT model not found. Download hubert.onnx to:\n"
                + hubert_onnx_path());
        return;
    }
    if (!rmvpe_onnx_exists()) {
        set_err("RMVPE pitch model not found. Download rmvpe.onnx to:\n"
                + rmvpe_onnx_path());
        return;
    }

    // ── One-time voice ONNX export (C++, no Python required) ─────────────────
    // A .onnx model is used as-is (standard RVC signature; third-party exports
    // work natively). A .pth model is exported once; re-export if the sidecar
    // lacks the current vc_version (older exports had graph bugs).
    std::string stem = model_path.substr(0, model_path.rfind('.'));
    std::string voice_onnx = stem + ".onnx";
    bool is_native_onnx = model_path.size() > 5 &&
                          model_path.compare(model_path.size() - 5, 5, ".onnx") == 0;
    bool need_export = !is_native_onnx && !fs::exists(voice_onnx);
    if (!is_native_onnx && !need_export) {
        std::ifstream jf(stem + ".json");
        std::string js((std::istreambuf_iterator<char>(jf)), {});
        if (js.find("\"vc_version\":5") == std::string::npos) need_export = true;
    }
    if (need_export) {
        set_prog(0.05f, "exporting model");
        PthModel pth = pth_open(model_path);
        if (!pth.err.empty()) { set_err("Failed to read model: " + pth.err); return; }
        std::string conv_err = pth_to_onnx(pth, voice_onnx);
        pth_close(pth);
        if (!conv_err.empty()) { set_err("Model export failed: " + conv_err); return; }
        set_prog(0.20f, "model exported");
    }

    // ── Decode source audio ───────────────────────────────────────────────────
    set_prog(0.60f, "converting");
    std::string wav_in = out_path + ".src.wav";
    std::string dec_cmd = "ffmpeg -hide_banner -loglevel error -y"
                          " -i \"" + source_path + "\""
                          " -vn -ar 44100 -ac 1 \"" + wav_in + "\" 2>/dev/null";
    if (pms_system(dec_cmd.c_str()) != 0 || !fs::exists(wav_in)) {  // NOLINT
        set_err("Failed to decode source audio");
        return;
    }

    // ── Inference ─────────────────────────────────────────────────────────────
    std::string err = vc_onnx_convert(wav_in, voice_onnx, out_path,
        f0_semitones, f0_auto,
        [&](float p, const std::string& msg) {
            set_prog(0.62f + p * 0.38f, "converting:" + msg);
        });
    fs::remove(wav_in);
    if (!err.empty()) { set_err(err); return; }

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
              const std::string& model_path, int f0_semitones, bool f0_auto)
{
    if (track_idx < 0 || track_idx >= (int)state.tracks.size()) return;
    auto& track = state.tracks[track_idx];
    if (clip_idx < 0 || clip_idx >= (int)track.clips.size()) return;
    Clip& clip = track.clips[clip_idx];
    // Audio clips convert their source; record bricks convert the selected take.
    std::string source;
    if (clip.clip_type == ClipType::Audio && !clip.text.empty())
        source = clip.text;
    else if (clip.clip_type == ClipType::Record &&
             clip.rec_take_sel >= 0 &&
             clip.rec_take_sel < (int)clip.rec_takes.size())
        source = clip.rec_takes[(size_t)clip.rec_take_sel];
    if (source.empty()) return;

    uint64_t h = std::hash<std::string>{}(source + model_path +
                 std::to_string(f0_semitones) + (f0_auto ? "a" : "m"));
    std::string out_path = "/tmp/pms_vc_" + std::to_string(h) + ".wav";

    clip.vc_status    = VcStatus::Processing;
    clip.vc_progress  = 0.f;
    clip.vc_out_path.clear();
    clip.vc_model_used = model_path;
    clip.vc_error.clear();

    auto data      = std::make_shared<VcJobData>();
    data->out_path = out_path;

    VcJob job;
    job.data      = data;
    job.track_idx = track_idx;
    job.clip_idx  = clip_idx;
    job.thread    = std::thread(run_job, data, source, model_path, out_path,
                                f0_semitones, f0_auto);

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

void vc_reconcile(AppState& state)
{
    // A voice-convert FX brick that targets clip c: same track, overlapping c,
    // with a model loaded. Mirrors the trigger's overlap test in
    // audio_fx_settings_ui (single-FX brick: clip.fx_type; AudioMultiFX brick:
    // any AudioVoiceConvert entry in its fx_chain — fx_chain entries are Clips).
    auto is_vc_fx = [](const Clip& b) -> bool {
        if (b.fx_type == FXType::AudioVoiceConvert &&
            !b.audio_fx.voice_model_path.empty()) return true;
        if (b.clip_type == ClipType::AudioMultiFX)
            for (const Clip& se : b.fx_chain)
                if (se.fx_type == FXType::AudioVoiceConvert &&
                    !se.audio_fx.voice_model_path.empty()) return true;
        return false;
    };

    for (auto& tr : state.tracks) {
        for (auto& c : tr.clips) {
            // Only audio clips and record takes ever carry a conversion.
            bool convertible =
                (c.clip_type == ClipType::Audio  && !c.text.empty()) ||
                (c.clip_type == ClipType::Record && c.rec_take_sel >= 0 &&
                 c.rec_take_sel < (int)c.rec_takes.size());
            if (!convertible) continue;
            if (c.vc_status == VcStatus::Idle && c.vc_out_path.empty()) continue;
            if (c.vc_status == VcStatus::Processing) continue;  // let the job land first

            // Still targeted? Either the clip's own per-clip VC, or a VC brick
            // overlapping it on the same track.
            bool active = c.audio_fx.voice_convert_on &&
                          !c.audio_fx.voice_model_path.empty();
            for (const Clip& b : tr.clips) {
                if (active) break;
                if (&b == &c) continue;
                if (b.end <= c.start || b.start >= c.end) continue;  // no overlap
                if (is_vc_fx(b)) active = true;
            }
            if (active) continue;

            // No VC FX left → drop the substitution. Playback/export/UI all key
            // off vc_status, so they revert to the clip's own source on their own.
            // The /tmp WAV is left on disk; undo restores this from history.
            c.vc_status = VcStatus::Idle;
            c.vc_out_path.clear();
            c.vc_model_used.clear();
            c.vc_error.clear();
            c.vc_progress = 0.f;
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
