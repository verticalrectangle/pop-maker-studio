#include "bg_remove.h"
#include "paths.h"
#include "proxy.h"
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

// ── u2net helpers ─────────────────────────────────────────────────────────────

static std::string u2net_model_path() {
    std::string dir  = app_models_dir();
    // Prefer the FULL u2net_human_seg model — far more accurate on dark / low-contrast
    // regions (dark clothing, hair, shadows) than the lightweight "p" variant, which
    // under-segments them and cuts them as background. Fall back to the lite model
    // only when the full one isn't bundled.
    std::string full = dir + "/u2net_human_seg.onnx";
    if (fs::exists(full)) return full;
    return dir + "/u2netp_human_seg.onnx";
}

static const float U2NET_MEAN[3] = {0.485f, 0.456f, 0.406f};
static const float U2NET_STD[3]  = {0.229f, 0.224f, 0.225f};
static const int   U2NET_SIZE    = 320;

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
                // Normalize and store in CHW format
                float n = (v / 255.f - U2NET_MEAN[c]) / U2NET_STD[c];
                dst[c * dw * dh + dy * dw + dx] = n;
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
    std::string model = u2net_model_path();
    if (!fs::exists(model)) {
        std::lock_guard<std::mutex> lk(data->mu);
        data->error_msg = "u2net model not found: " + model +
                          "\nPlace the models/ folder next to the binary.";
        data->status.store(2);
        return;
    }

    Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "rembg");
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
    auto in_name  = session->GetInputNameAllocated(0, alloc);
    auto out_name = session->GetOutputNameAllocated(0, alloc);
    const char* in_names[]  = {in_name.get()};
    const char* out_names[] = {out_name.get()};

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

    // Probe whether the model supports dynamic batch sizes.
    bool dynamic_batch = false;
    {
        int probe_n = std::min(8, (int)std::thread::hardware_concurrency());
        if (probe_n > 1) {
            std::vector<float> probe(2 * 3 * U2NET_SIZE * U2NET_SIZE, 0.f);
            int64_t pshape[] = {2, 3, U2NET_SIZE, U2NET_SIZE};
            try {
                Ort::Value pt = Ort::Value::CreateTensor<float>(
                    mem_info, probe.data(), probe.size(), pshape, 4);
                session->Run(Ort::RunOptions{nullptr}, in_names, &pt, 1, out_names, 1);
                dynamic_batch = true;
            } catch (...) {}
        }
    }

    if (dynamic_batch) {
        // Model supports variable batch — run all frames in batches of N.
        int BATCH = std::min(8, (int)std::thread::hardware_concurrency());
        std::vector<float> input_batch((size_t)BATCH * 3 * U2NET_SIZE * U2NET_SIZE);

        for (int i = 0; i < total; i += BATCH) {
            if (g_shutdown.load()) { fclose(mjpeg_f); fs::remove_all(tmpdir); return; }
            const int bs = std::min(BATCH, total - i);

            std::vector<int> frame_ws(bs, 0), frame_hs(bs, 0);
            for (int b = 0; b < bs; ++b) {
                int w, h, ch;
                uint8_t* img = stbi_load(frames[i + b].string().c_str(), &w, &h, &ch, 3);
                if (!img) continue;
                frame_ws[b] = w; frame_hs[b] = h;
                bilinear_resize_rgb(img, w, h,
                    input_batch.data() + (size_t)b * 3 * U2NET_SIZE * U2NET_SIZE,
                    U2NET_SIZE, U2NET_SIZE);
                stbi_image_free(img);
            }

            int64_t shape[] = {bs, 3, U2NET_SIZE, U2NET_SIZE};
            Ort::Value in_tensor = Ort::Value::CreateTensor<float>(
                mem_info, input_batch.data(),
                (size_t)bs * 3 * U2NET_SIZE * U2NET_SIZE, shape, 4);

            std::vector<Ort::Value> outputs;
            try {
                outputs = session->Run(Ort::RunOptions{nullptr},
                                       in_names, &in_tensor, 1, out_names, 1);
            } catch (...) { data->progress.store((float)(i + bs) / total); continue; }

            float* mask_data = outputs[0].GetTensorMutableData<float>();
            auto mshape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
            int mh = (int)mshape[mshape.size() - 2];
            int mw = (int)mshape[mshape.size() - 1];
            size_t mask_stride = (size_t)mh * mw;

            for (int b = 0; b < bs; ++b) {
                if (frame_ws[b] == 0) continue;
                int w = frame_ws[b], h = frame_hs[b];
                std::vector<uint8_t> alpha(w * h);
                bilinear_resize_mask(mask_data + b * mask_stride, mw, mh, alpha.data(), w, h);
                gaussian_blur_mask(alpha.data(), w, h);
                JpegBuf buf;
                stbi_write_jpg_to_func(JpegBuf::cb, &buf, w, h, 1, alpha.data(), 90);
                fwrite(buf.data.data(), 1, buf.data.size(), mjpeg_f);
                fflush(mjpeg_f);
            }
            data->progress.store((float)(i + bs) / total);
        }
    } else {
        // Fixed batch=1 model — run N parallel sessions, each processing a different frame.
        // Each session is single-threaded to avoid oversubscription. HALF the
        // cores, niced: one-per-core saturated the machine and the app itself
        // (UI, decode, audio) visibly stuttered while masks generated.
        const int N = std::max(1, (int)std::thread::hardware_concurrency() / 2);
        Ort::SessionOptions wopts;
        wopts.SetIntraOpNumThreads(1);
        wopts.SetInterOpNumThreads(1);
        wopts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        std::vector<std::unique_ptr<Ort::Session>> sessions(N);
        sessions[0] = std::move(session);  // reuse already-loaded session
        for (int i = 1; i < N; ++i) {
            try { sessions[i] = std::make_unique<Ort::Session>(env, model.c_str(), wopts); }
            catch (...) { sessions.resize(i); break; }
        }

        // Results stored in order, written sequentially after all workers finish.
        std::vector<std::vector<uint8_t>> results(total);
        std::atomic<int> next_frame{0};
        std::atomic<bool> aborted{false};

        auto worker = [&](Ort::Session* sess) {
            // Background QoS: the live app outranks mask generation.
            setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
            auto wm = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            std::vector<float> inp(3 * U2NET_SIZE * U2NET_SIZE);
            while (true) {
                if (g_shutdown.load()) { aborted.store(true); return; }
                int i = next_frame.fetch_add(1);
                if (i >= total) return;

                int w, h, ch;
                uint8_t* img = stbi_load(frames[i].string().c_str(), &w, &h, &ch, 3);
                if (!img) { data->progress.store((float)i / total); continue; }
                bilinear_resize_rgb(img, w, h, inp.data(), U2NET_SIZE, U2NET_SIZE);
                stbi_image_free(img);

                int64_t shape[] = {1, 3, U2NET_SIZE, U2NET_SIZE};
                Ort::Value in_t = Ort::Value::CreateTensor<float>(wm, inp.data(), inp.size(), shape, 4);
                std::vector<Ort::Value> outs;
                try { outs = sess->Run(Ort::RunOptions{nullptr}, in_names, &in_t, 1, out_names, 1); }
                catch (...) { data->progress.store((float)i / total); continue; }

                float* md = outs[0].GetTensorMutableData<float>();
                auto ms = outs[0].GetTensorTypeAndShapeInfo().GetShape();
                int mh = (int)ms[ms.size()-2], mw2 = (int)ms[ms.size()-1];

                std::vector<uint8_t> alpha(w * h);
                bilinear_resize_mask(md, mw2, mh, alpha.data(), w, h);
                gaussian_blur_mask(alpha.data(), w, h);
                JpegBuf buf;
                stbi_write_jpg_to_func(JpegBuf::cb, &buf, w, h, 1, alpha.data(), 90);
                results[i] = std::move(buf.data);
                data->progress.store((float)next_frame.load() / total);
            }
        };

        std::vector<std::thread> threads;
        threads.reserve(sessions.size());
        for (auto& s : sessions) threads.emplace_back(worker, s.get());
        for (auto& t : threads) t.join();

        if (aborted.load()) { fclose(mjpeg_f); fs::remove_all(tmpdir); return; }

        for (auto& res : results) {
            if (!res.empty()) fwrite(res.data(), 1, res.size(), mjpeg_f);
        }
    }

    fclose(mjpeg_f);
    // Remove stale seek-table so it gets rebuilt on next read
    fs::remove(output_dir + "/bg_masks.idx");
    fs::remove_all(tmpdir);
    data->status.store(1);
}

// ── Public API ────────────────────────────────────────────────────────────────

// Brick-only background removal: ensure a RemoveBackground BodyFX brick overlaps the
// video clip on its track — the brick is the sole consumer of the masks now. Mirrors
// the brick the MCP add_clip(type=body_fx) makes (uncoupled, spanning the clip). No-op
// when one already overlaps.
static void ensure_remove_bg_brick(AppState& state, int ti, int ci) {
    if (ti < 0 || ti >= (int)state.tracks.size()) return;
    auto& clips = state.tracks[(size_t)ti].clips;
    if (ci < 0 || ci >= (int)clips.size()) return;
    float cs = clips[(size_t)ci].start, ce = clips[(size_t)ci].end;
    for (auto& d : clips) {
        if (d.start >= ce || d.end <= cs) continue;            // no overlap
        if (d.clip_type == ClipType::BodyFX &&
            d.body_fx_type == BodyFXType::RemoveBackground)
            return;                                            // standalone brick exists
        if (d.clip_type == ClipType::MultiFX)                  // or a glass-chain sub-effect
            for (auto& se : d.fx_chain)
                if (se.clip_type == ClipType::BodyFX &&
                    se.body_fx_type == BodyFXType::RemoveBackground)
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

// ── rembg/u2net model management ─────────────────────────────────────────────

static std::atomic<int>  s_install_status{0};  // 0=idle 1=running 2=done 3=failed
static std::string       s_install_error;
static std::mutex        s_install_mu;

bool rembg_is_installed(const std::string& /*python_path*/) {
    return fs::exists(u2net_model_path());
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
