#include "bg_remove.h"
#include "paths.h"
#include "proxy.h"
#include "body_fx.h"   // invalidate/evict the mask caches when masks regenerate
#include <onnxruntime_cxx_api.h>
#include <filesystem>
#include <map>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include "stb_image.h"
#include "stb_image_write.h"
#include <unistd.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <sys/wait.h>

namespace fs = std::filesystem;

// ── ffprobe helper — no shell, safe for Unicode/special-char paths ────────────

static float probe_fps(const std::string& path) {
    std::string file_arg = "file:" + path;
    const char* argv[] = {"ffprobe", "-v", "error", "-select_streams", "v:0",
                          "-show_entries", "stream=r_frame_rate",
                          "-of", "csv=p=0", file_arg.c_str(), nullptr};
    int pipefd[2];
    if (pipe(pipefd) != 0) return 30.f;
    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) { dup2(devnull, STDIN_FILENO); dup2(devnull, STDERR_FILENO); close(devnull); }
        execvp("ffprobe", const_cast<char**>(argv));
        _exit(127);
    }
    close(pipefd[1]);
    char buf[64] = {};
    (void)read(pipefd[0], buf, sizeof(buf) - 1);
    close(pipefd[0]);
    waitpid(pid, nullptr, 0);
    int nr = 0, dr = 0;
    if (sscanf(buf, "%d/%d", &nr, &dr) == 2 && dr > 0) return (float)nr / dr;
    float v = (float)atof(buf);
    return (v > 0.f && v < 1000.f) ? v : 30.f;
}

// ── Job data shared between dispatch thread and poll ──────────────────────────

struct JobData {
    std::atomic<float> progress{0.f};
    std::atomic<int>   status{0};   // 0=running 1=done 2=error
    std::string        error_msg;
    std::mutex         mu;
    bool               append_mode = false;  // open bg_masks.mjpeg in "ab" mode
};

struct BgJob {
    std::shared_ptr<JobData> data;
    std::thread              thread;
};

static std::map<std::string, BgJob> g_jobs;
static std::mutex                   g_jobs_mu;

// ── Path helpers ──────────────────────────────────────────────────────────────

std::string bg_remove_proxy_dir(const std::string& video_path) {
    return cache_path(video_path, "_bg_masks");
}

std::string bg_remove_hires_dir(const std::string& video_path) {
    return cache_path(video_path, "_bg_hires");
}

float bg_remove_read_fps(const std::string& mask_dir) {
    std::string fp = mask_dir + "/fps.txt";
    FILE* f = fopen(fp.c_str(), "r");
    if (!f) return 30.f;
    float fps = 30.f;
    fscanf(f, "%f", &fps);
    fclose(f);
    return (fps > 0.f && fps < 1000.f) ? fps : 30.f;
}

int bg_remove_read_start_frame(const std::string& mask_dir) {
    std::string fp = mask_dir + "/start_frame.txt";
    FILE* f = fopen(fp.c_str(), "r");
    if (!f) return 0;
    int sf = 0;
    fscanf(f, "%d", &sf);
    fclose(f);
    return sf;
}

int bg_remove_read_frame_count(const std::string& mask_dir) {
    std::string idx = mask_dir + "/bg_masks.idx";
    // Build .idx from bg_masks.mjpeg if missing
    if (!fs::exists(idx)) {
        std::string mjpeg = mask_dir + "/bg_masks.mjpeg";
        if (!fs::exists(mjpeg)) return 0;
        FILE* mf = fopen(mjpeg.c_str(), "rb");
        if (!mf) return 0;
        std::vector<uint64_t> offsets;
        offsets.reserve(8192);
        uint8_t carry[2] = {0, 0};
        bool has_carry = false;
        uint8_t chunk[65536];
        int64_t abs_off = 0;
        while (true) {
            size_t nr = fread(chunk, 1, sizeof(chunk), mf);
            if (nr == 0) break;
            if (has_carry && nr > 0) {
                if (carry[0] == 0xFF && carry[1] == 0xD8 && chunk[0] == 0xFF)
                    offsets.push_back((uint64_t)(abs_off - 2));
            }
            for (size_t i = 0; i + 2 < nr; ++i) {
                if (chunk[i] == 0xFF && chunk[i+1] == 0xD8 && chunk[i+2] == 0xFF)
                    offsets.push_back((uint64_t)(abs_off + (int64_t)i));
            }
            if (nr >= 2) { carry[0] = chunk[nr-2]; carry[1] = chunk[nr-1]; }
            else if (nr == 1) { carry[0] = 0; carry[1] = chunk[0]; }
            has_carry = (nr > 0);
            abs_off += (int64_t)nr;
        }
        fclose(mf);
        if (!offsets.empty()) {
            FILE* idxf = fopen(idx.c_str(), "wb");
            if (idxf) {
                uint32_t cnt = (uint32_t)offsets.size();
                fwrite(&cnt, sizeof(cnt), 1, idxf);
                fwrite(offsets.data(), sizeof(uint64_t), cnt, idxf);
                fclose(idxf);
            }
        }
    }
    FILE* idxf = fopen(idx.c_str(), "rb");
    if (!idxf) return 0;
    uint32_t cnt = 0;
    fread(&cnt, sizeof(cnt), 1, idxf);
    fclose(idxf);
    return (int)cnt;
}

// ── RobustVideoMatting (RVM) helpers ──────────────────────────────────────────

static std::string rvm_model_path() {
    return app_models_dir() + "/rvm_mobilenetv3_fp32.onnx";
}

// RVM is a recurrent video matting net: it takes the frame + 4 recurrent state
// tensors + a downsample_ratio, and returns foreground, alpha, and updated
// states. It matts at the input resolution and downsamples internally by
// downsample_ratio for the encoder. We feed a frame scaled to long-edge RVM_LONG
// (aspect-preserved, dims /16-aligned so the encoder's four /2 downsamples divide
// cleanly), keep RATIO=1.0 (encoder runs at the input res), and upscale the
// returned alpha back to native. RATIO<1 wrecked hard subjects: at a 512 input,
// ratio 0.5 → encoder at 256 detected NOTHING on a backlit profile; ratio 1.0 at
// long=720 gave a solid head+shoulders matte (measured: coverage 0.25 vs 0.00).
// Temporal recurrence is why this must run frames strictly in order.
static const int   RVM_LONG  = 720;
static const float RVM_RATIO = 1.0f;

// Bilinear resize of RGB image (src_w*src_h*3) → (dst_w*dst_h*3)
static void bilinear_resize_rgb(const uint8_t* src, int sw, int sh,
                                 float* dst, int dw, int dh) {
    float xs = (float)sw / dw, ys = (float)sh / dh;
    for (int dy = 0; dy < dh; dy++) {
        float fy = (dy + 0.5f) * ys - 0.5f;
        int y0 = (int)fy; int y1 = y0 + 1;
        float wy = fy - y0;
        y0 = std::max(0, std::min(y0, sh-1));
        y1 = std::max(0, std::min(y1, sh-1));
        for (int dx = 0; dx < dw; dx++) {
            float fx = (dx + 0.5f) * xs - 0.5f;
            int x0 = (int)fx; int x1 = x0 + 1;
            float wx = fx - x0;
            x0 = std::max(0, std::min(x0, sw-1));
            x1 = std::max(0, std::min(x1, sw-1));
            for (int c = 0; c < 3; c++) {
                float v = (1-wy)*((1-wx)*src[(y0*sw+x0)*3+c]
                                  + wx * src[(y0*sw+x1)*3+c])
                        +    wy *((1-wx)*src[(y1*sw+x0)*3+c]
                                  + wx * src[(y1*sw+x1)*3+c]);
                // RVM wants src in [0,1], RGB, CHW (no ImageNet mean/std).
                dst[c * dw * dh + dy * dw + dx] = v / 255.f;
            }
        }
    }
}

// Bilinear resize of float mask (sw*sh) → uint8 mask (dw*dh)
static void bilinear_resize_mask(const float* src, int sw, int sh,
                                  uint8_t* dst, int dw, int dh) {
    float xs = (float)sw / dw, ys = (float)sh / dh;
    for (int dy = 0; dy < dh; dy++) {
        float fy = (dy + 0.5f) * ys - 0.5f;
        int y0 = (int)fy; int y1 = y0 + 1;
        float wy = fy - y0;
        y0 = std::max(0, std::min(y0, sh-1));
        y1 = std::max(0, std::min(y1, sh-1));
        for (int dx = 0; dx < dw; dx++) {
            float fx = (dx + 0.5f) * xs - 0.5f;
            int x0 = (int)fx; int x1 = x0 + 1;
            float wx = fx - x0;
            x0 = std::max(0, std::min(x0, sw-1));
            x1 = std::max(0, std::min(x1, sw-1));
            float v = (1-wy)*((1-wx)*src[y0*sw+x0] + wx*src[y0*sw+x1])
                    +    wy *((1-wx)*src[y1*sw+x0] + wx*src[y1*sw+x1]);
            dst[dy * dw + dx] = (uint8_t)(std::max(0.f, std::min(1.f, v)) * 255.f);
        }
    }
}

// Simple separable Gaussian blur (radius ~1px) on uint8 mask in-place
static void gaussian_blur_mask(uint8_t* data, int w, int h) {
    static const float k[5] = {0.0625f, 0.25f, 0.375f, 0.25f, 0.0625f};
    std::vector<float> tmp(w * h);
    // Horizontal pass
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float s = 0.f;
            for (int d = -2; d <= 2; d++) {
                int sx = std::max(0, std::min(x+d, w-1));
                s += k[d+2] * data[y*w+sx];
            }
            tmp[y*w+x] = s;
        }
    }
    // Vertical pass
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float s = 0.f;
            for (int d = -2; d <= 2; d++) {
                int sy = std::max(0, std::min(y+d, h-1));
                s += k[d+2] * tmp[sy*w+x];
            }
            data[y*w+x] = (uint8_t)std::max(0.f, std::min(255.f, s));
        }
    }
}

// JPEG write callback → buffer
struct JpegBuf {
    std::vector<uint8_t> data;
    static void cb(void* ctx, void* ptr, int sz) {
        auto* b = static_cast<JpegBuf*>(ctx);
        auto* p = static_cast<uint8_t*>(ptr);
        b->data.insert(b->data.end(), p, p + sz);
    }
};

// ── Background worker ─────────────────────────────────────────────────────────

static void run_job(std::shared_ptr<JobData> data,
                    const std::string& input_path,
                    const std::string& output_dir,
                    float start_time,
                    float /*duration*/,
                    int start_frame,
                    int num_frames) {
    // Background QoS for the whole job (covers the batch path too — worker
    // threads inherit): the live app outranks mask generation.
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
    // Probe fps
    float fps = probe_fps(input_path);
    if (fps <= 0.f || fps > 1000.f) fps = 30.f;

    fs::create_directories(output_dir);
    if (!data->append_mode) {
        {
            FILE* f = fopen((output_dir + "/fps.txt").c_str(), "w");
            if (f) { fprintf(f, "%.6f\n", fps); fclose(f); }
        }
        {
            int sf = (start_frame >= 0) ? start_frame : (int)(start_time * fps);
            FILE* f = fopen((output_dir + "/start_frame.txt").c_str(), "w");
            if (f) { fprintf(f, "%d\n", sf); fclose(f); }
        }
    }

    // Load ONNX model
    std::string model = rvm_model_path();
    if (!fs::exists(model)) {
        std::lock_guard<std::mutex> lk(data->mu);
        data->error_msg = "RVM model not found: " + model +
                          "\nPlace the models/ folder next to the binary.";
        data->status.store(2);
        return;
    }

    Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "rvm");
    Ort::SessionOptions opts;
    // Half the cores: full-width inference starved the live app while
    // masks generated (the 'remove background made everything laggy' report).
    const int n_inf_threads = std::max(1, (int)std::thread::hardware_concurrency() / 2);
    opts.SetIntraOpNumThreads(n_inf_threads);
    opts.SetInterOpNumThreads(1);
    opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

    std::unique_ptr<Ort::Session> session;
    try {
        session = std::make_unique<Ort::Session>(env, model.c_str(), opts);
    } catch (const Ort::Exception& e) {
        std::lock_guard<std::mutex> lk(data->mu);
        data->error_msg = std::string("ONNX load failed: ") + e.what();
        data->status.store(2);
        return;
    }

    Ort::AllocatorWithDefaultOptions alloc;
    // RVM: inputs  = src, r1i, r2i, r3i, r4i, downsample_ratio
    //      outputs = fgr, pha, r1o, r2o, r3o, r4o
    std::vector<Ort::AllocatedStringPtr> in_holders, out_holders;
    std::vector<const char*> in_names, out_names;
    for (size_t i = 0; i < session->GetInputCount(); ++i) {
        in_holders.push_back(session->GetInputNameAllocated(i, alloc));
        in_names.push_back(in_holders.back().get());
    }
    for (size_t i = 0; i < session->GetOutputCount(); ++i) {
        out_holders.push_back(session->GetOutputNameAllocated(i, alloc));
        out_names.push_back(out_holders.back().get());
    }

    // Extract frames to temp dir using frame-number selection for accuracy
    std::string tmpdir = "/tmp/pms_bg_" + std::to_string(
        std::hash<std::string>{}(input_path + std::to_string(start_frame)));
    fs::create_directories(tmpdir);

    {
        std::string file_arg    = "file:" + input_path;
        std::string out_pattern = tmpdir + "/%06d.jpg";
        std::string select_filter, ss_val;

        std::vector<const char*> fargv = {"ffmpeg", "-hide_banner", "-loglevel", "error",
                                           "-i", file_arg.c_str()};
        if (num_frames > 0 && start_frame >= 0) {
            int ef = start_frame + num_frames - 1;
            select_filter = "select='between(n," + std::to_string(start_frame)
                            + "," + std::to_string(ef) + ")'";
            fargv.insert(fargv.end(), {"-vf", select_filter.c_str(), "-vsync", "vfr"});
        } else if (start_time > 0.001f) {
            char ss[32]; snprintf(ss, sizeof(ss), "%.6f", start_time);
            ss_val = ss;
            fargv.insert(fargv.end(), {"-ss", ss_val.c_str()});
        }
        fargv.insert(fargv.end(), {"-f", "image2", out_pattern.c_str(), nullptr});

        pid_t pid = fork();
        if (pid == 0) {
            int devnull = open("/dev/null", O_RDWR);
            if (devnull >= 0) { dup2(devnull, STDIN_FILENO); dup2(devnull, STDOUT_FILENO); dup2(devnull, STDERR_FILENO); close(devnull); }
            execvp("ffmpeg", const_cast<char**>(fargv.data()));
            _exit(127);
        }
        waitpid(pid, nullptr, 0);
    }

    // Collect frames
    std::vector<fs::path> frames;
    for (auto& e : fs::directory_iterator(tmpdir)) {
        if (e.path().extension() == ".jpg")
            frames.push_back(e.path());
    }
    std::sort(frames.begin(), frames.end());

    if (frames.empty()) {
        fs::remove_all(tmpdir);
        std::lock_guard<std::mutex> lk(data->mu);
        data->error_msg = "No frames extracted from proxy";
        data->status.store(2);
        return;
    }

    int total = (int)frames.size();
    std::string mjpeg_path = output_dir + "/bg_masks.mjpeg";
    FILE* mjpeg_f = fopen(mjpeg_path.c_str(), data->append_mode ? "ab" : "wb");
    if (!mjpeg_f) {
        fs::remove_all(tmpdir);
        std::lock_guard<std::mutex> lk(data->mu);
        data->error_msg = "Cannot create output MJPEG at " + mjpeg_path;
        data->status.store(2);
        return;
    }

    auto mem_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    // Background QoS: the live app outranks mask generation. The session already
    // caps intra-op threads at half the cores (see above); nice this worker too.
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);

    // ── RVM sequential matting ────────────────────────────────────────────────
    // RVM is RECURRENT: each frame's four output states (r1o..r4o) feed the next
    // frame's inputs (r1i..r4i), which is what makes the matte temporally stable
    // (no per-frame flicker) — but it forces strictly in-order processing (no
    // batching / no parallel sessions like u2net). Init states are 1x1x1x1 zeros;
    // ORT grows them to the right shape on the first run. A sub-range restart just
    // costs a few warm-up frames at its start.
    int   ww = 0, wh = 0;                    // model working size (from first frame)
    std::vector<float>   src_buf;            // [3*wh*ww] CHW [0,1]
    std::vector<float>   rstate[4];          // recurrent state data, carried frame→frame
    std::vector<int64_t> rshape[4];
    for (int k = 0; k < 4; ++k) { rstate[k].assign(1, 0.f); rshape[k] = {1, 1, 1, 1}; }
    float   ratio_val = RVM_RATIO;
    int64_t ratio_shape[1] = {1};

    for (int i = 0; i < total; ++i) {
        if (g_shutdown.load()) { fclose(mjpeg_f); fs::remove_all(tmpdir); return; }

        int w, h, ch;
        uint8_t* img = stbi_load(frames[i].string().c_str(), &w, &h, &ch, 3);
        if (!img) { data->progress.store((float)(i + 1) / total); continue; }

        if (ww == 0) {                       // fix working size once (all frames same size)
            float sc = (float)RVM_LONG / std::max(w, h);
            auto a16 = [](int v) { return std::max(16, (v + 8) / 16 * 16); };
            ww = a16((int)std::lround(w * sc));
            wh = a16((int)std::lround(h * sc));
            src_buf.resize((size_t)3 * ww * wh);
        }

        bilinear_resize_rgb(img, w, h, src_buf.data(), ww, wh);   // → [0,1] CHW
        stbi_image_free(img);

        // inputs: src, r1i..r4i, downsample_ratio
        std::vector<Ort::Value> ins;
        int64_t src_shape[4] = {1, 3, wh, ww};
        ins.push_back(Ort::Value::CreateTensor<float>(
            mem_info, src_buf.data(), src_buf.size(), src_shape, 4));
        for (int k = 0; k < 4; ++k)
            ins.push_back(Ort::Value::CreateTensor<float>(
                mem_info, rstate[k].data(), rstate[k].size(),
                rshape[k].data(), (size_t)rshape[k].size()));
        ins.push_back(Ort::Value::CreateTensor<float>(
            mem_info, &ratio_val, 1, ratio_shape, 1));

        std::vector<Ort::Value> outs;
        try {
            outs = session->Run(Ort::RunOptions{nullptr},
                                in_names.data(), ins.data(), ins.size(),
                                out_names.data(), out_names.size());
        } catch (...) { data->progress.store((float)(i + 1) / total); continue; }

        // outputs: fgr, pha, r1o..r4o — pha is the [0,1] alpha at working size.
        float* pha = outs[1].GetTensorMutableData<float>();
        auto ps = outs[1].GetTensorTypeAndShapeInfo().GetShape();
        int ph = (int)ps[ps.size() - 2], pw = (int)ps[ps.size() - 1];

        std::vector<uint8_t> alpha((size_t)w * h);
        bilinear_resize_mask(pha, pw, ph, alpha.data(), w, h);
        gaussian_blur_mask(alpha.data(), w, h);
        JpegBuf buf;
        stbi_write_jpg_to_func(JpegBuf::cb, &buf, w, h, 1, alpha.data(), 90);
        fwrite(buf.data.data(), 1, buf.data.size(), mjpeg_f);
        fflush(mjpeg_f);

        // carry recurrent states forward (r1o..r4o = outs[2..5])
        for (int k = 0; k < 4; ++k) {
            auto rs = outs[2 + k].GetTensorTypeAndShapeInfo().GetShape();
            size_t n = 1; for (auto d : rs) n *= (size_t)d;
            const float* rd = outs[2 + k].GetTensorData<float>();
            rstate[k].assign(rd, rd + n);
            rshape[k].assign(rs.begin(), rs.end());
        }

        data->progress.store((float)(i + 1) / total);
    }

    fclose(mjpeg_f);
    // Remove stale seek-table so it gets rebuilt on next read
    fs::remove(output_dir + "/bg_masks.idx");
    fs::remove_all(tmpdir);
    data->status.store(1);
}

// ── Public API ────────────────────────────────────────────────────────────────

// Ensure SOME body-FX brick overlaps the video clip to consume the masks; if none
// does, add a default RemoveBackground cutout brick (spanning the clip, uncoupled —
// mirrors the MCP add_clip(type=body_fx) brick). ANY body-FX brick (NeonOutline,
// Hologram, …) already samples the masks, so the cutout is only auto-added when the
// clip has no body-FX at all — e.g. the bare-clip "Remove Background" button. This
// stops adding a non-cutout body-FX from also spawning an unwanted RemoveBackground.
static void ensure_remove_bg_brick(AppState& state, int ti, int ci) {
    if (ti < 0 || ti >= (int)state.tracks.size()) return;
    auto& clips = state.tracks[(size_t)ti].clips;
    if (ci < 0 || ci >= (int)clips.size()) return;
    float cs = clips[(size_t)ci].start, ce = clips[(size_t)ci].end;
    for (auto& d : clips) {
        if (d.start >= ce || d.end <= cs) continue;            // no overlap
        if (d.clip_type == ClipType::BodyFX)                   // any standalone body-FX brick
            return;                                            // already consumes the masks
        if (d.clip_type == ClipType::MultiFX)                  // or a glass chain holding one
            for (auto& se : d.fx_chain)
                if (se.clip_type == ClipType::BodyFX)
                    return;
    }
    Clip brick;
    brick.clip_type      = ClipType::BodyFX;
    brick.body_fx_type   = BodyFXType::RemoveBackground;
    brick.body_fx_amount = 1.f;
    brick.start = cs;
    brick.end   = ce;
    clips.push_back(std::move(brick));
    fprintf(stderr, "[bg] auto-added RemoveBackground brick on track %d clip %d\n", ti, ci);
}

void bg_remove_start(AppState& state, int track_idx, int clip_idx) {
    if (track_idx < 0 || track_idx >= (int)state.tracks.size()) return;
    auto& track = state.tracks[track_idx];
    if (clip_idx < 0 || clip_idx >= (int)track.clips.size()) return;
    Clip& clip = track.clips[clip_idx];
    if (!clip_is_videolike_type(clip.clip_type) || clip.text.empty()) return;

    // Background removal is keyed to the video clip (one mask set per source path),
    // so every body-FX brick on it shares the same masks. If a job is already
    // running, a second trigger — another body-FX brick, a double-click — must not
    // wipe the partial output and spawn a worker racing the first; the new brick
    // just rides the in-flight job. (There's no per-job cancel, so restarting
    // mid-flight would orphan the running worker; a re-run once it's Ready, or a
    // retry after Error, regenerates cleanly — those statuses fall through.)
    if (clip.bg_remove_status == BgRemoveStatus::Processing)
        return;

    std::string mjpeg = proxy_mjpeg_path(clip.text);
    if (!fs::exists(mjpeg)) {
        clip.bg_remove_status = BgRemoveStatus::WaitingForProxy;
        ensure_remove_bg_brick(state, track_idx, clip_idx);  // brick rides it even pre-proxy
        return;
    }

    std::string mdir = bg_remove_proxy_dir(clip.text);
    clip.bg_remove_mask_dir = mdir;
    clip.bg_remove_status   = BgRemoveStatus::Processing;
    clip.bg_remove_progress = 0.f;
    clip.bg_remove_error.clear();
    clip.bg_remove_on       = true;

    if (fs::exists(mdir)) {
        fs::remove(mdir + "/bg_masks.mjpeg");
        fs::remove(mdir + "/start_frame.txt");
    }
    // The cached seek table indexes the OLD mjpeg's byte offsets; drop it now so the
    // render rebuilds from the regenerated file (stale offsets read garbage = smudge).
    // No-GL, so safe regardless of caller thread; the poll re-invalidates on completion.
    body_fx_invalidate_mask_index(mdir);

    {
        std::lock_guard<std::mutex> lk(g_jobs_mu);
        g_jobs.erase(mdir);
    }

    auto data = std::make_shared<JobData>();

    float start_time = clip.in_point;
    float duration   = (clip.end - clip.start) * clip.speed;
    int start_frame = 0, num_frames = 0;
    {
        ProxyInfo pinfo;
        if (proxy_load(clip.text, pinfo) && pinfo.fps_num > 0 && pinfo.fps_den > 0) {
            start_frame = (int)((int64_t)(start_time * (double)pinfo.fps_num) / pinfo.fps_den);
            num_frames  = (int)((int64_t)(duration   * (double)pinfo.fps_num) / pinfo.fps_den) + 1;
        } else if (pinfo.fps > 0.0) {
            start_frame = (int)(start_time * pinfo.fps);
            num_frames  = (int)(duration   * pinfo.fps) + 1;
        }
    }

    BgJob job;
    job.data   = data;
    job.thread = std::thread(run_job, data, mjpeg, mdir,
                             start_time, duration, start_frame, num_frames);
    job.thread.detach();

    {
        std::lock_guard<std::mutex> lk(g_jobs_mu);
        g_jobs[mdir] = std::move(job);
    }

    // Guarantee the brick exists so the masks are actually applied (every removal
    // trigger flows through here; the MCP path adds the brick first → no-op).
    ensure_remove_bg_brick(state, track_idx, clip_idx);
}

void bg_remove_poll(AppState& state) {
    std::lock_guard<std::mutex> lk(g_jobs_mu);

    for (auto& [mdir, job] : g_jobs) {
        int   st   = job.data->status.load();
        float prog = job.data->progress.load();

        for (auto& track : state.tracks) {
            for (auto& clip : track.clips) {
                if (clip.bg_remove_mask_dir != mdir) continue;
                clip.bg_remove_progress = prog;
                if (st == 1) {
                    clip.bg_remove_status = BgRemoveStatus::Ready;
                } else if (st == 2) {
                    clip.bg_remove_status = BgRemoveStatus::Error;
                    std::lock_guard<std::mutex> lk2(job.data->mu);
                    clip.bg_remove_error = job.data->error_msg;
                }
            }
        }
        if (st == 1) {
            // Masks just finished regenerating — drop BOTH stale caches so the render
            // rebuilds from the new bg_masks.mjpeg. The new file's per-frame byte offsets
            // differ from the old one, so a stale seek table reads wrong ranges = the
            // garbage/smudge that "stays" until a process restart (this is the bug:
            // a Re-run / re-apply rewrote the masks but the caches kept old offsets).
            // bg_remove_poll runs on the main thread, so the GL evict is safe here.
            body_fx_invalidate_mask_index(mdir);
            body_fx_evict_mask_cache(mdir);
        }
    }

    for (auto it = g_jobs.begin(); it != g_jobs.end(); ) {
        if (it->second.data->status.load() != 0)
            it = g_jobs.erase(it);
        else
            ++it;
    }

    // Guarantee a brick for ANY clip with removal on + masks ready — covers projects
    // LOADED with the flag set but no brick (e.g. made on a pre-brick build), which
    // bg_remove_start never re-runs. Collect first (ensure mutates clips).
    std::vector<std::pair<int,int>> need;
    for (int ti = 0; ti < (int)state.tracks.size(); ++ti)
        for (int ci = 0; ci < (int)state.tracks[(size_t)ti].clips.size(); ++ci) {
            auto& c = state.tracks[(size_t)ti].clips[(size_t)ci];
            if (c.bg_remove_on && c.bg_remove_status == BgRemoveStatus::Ready)
                need.push_back({ti, ci});
        }
    for (auto& tc : need) ensure_remove_bg_brick(state, tc.first, tc.second);
}

void bg_remove_cancel_all() {
    std::lock_guard<std::mutex> lk(g_jobs_mu);
    g_jobs.clear();
}

// ── BodyFX solid brick mask generation ────────────────────────────────────────

// Job key for a BodyFX brick: "bfx:<track>:<clip>:<source_path>"


bool bg_remove_run_hires(const std::string& video_path,
                          const std::string& output_dir) {
    if (fs::exists(output_dir + "/fps.txt")) return true;

    // Synchronously process all frames of the original video at full res.
    // Used at export time — blocking call from render thread.
    auto data = std::make_shared<JobData>();

    float fps = probe_fps(video_path);
    if (fps <= 0.f || fps > 1000.f) fps = 30.f;

    // Delegate to run_job (synchronous: we block until done)
    run_job(data, video_path, output_dir, 0.f, 0.f, -1, -1);
    return data->status.load() == 1;
}

// ── RVM model management ─────────────────────────────────────────────────────

static std::atomic<int>  s_install_status{0};  // 0=idle 1=running 2=done 3=failed
static std::string       s_install_error;
static std::mutex        s_install_mu;

bool rembg_is_installed(const std::string& /*python_path*/) {
    return fs::exists(rvm_model_path());
}

void rembg_install_reset() {
    // no-op: model is bundled
}

void rembg_install_start(const std::string& /*python_path*/) {
    // no-op: model is bundled, cannot install
}

RembgInstallStatus rembg_install_status() {
    switch (s_install_status.load()) {
        case 1: return RembgInstallStatus::Running;
        case 2: return RembgInstallStatus::Done;
        case 3: return RembgInstallStatus::Failed;
        default: return RembgInstallStatus::Idle;
    }
}

std::string rembg_install_error() {
    std::lock_guard<std::mutex> lk(s_install_mu);
    return s_install_error;
}
