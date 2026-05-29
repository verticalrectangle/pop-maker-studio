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

namespace fs = std::filesystem;

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
    fs::path p(video_path);
    return (p.parent_path() / (p.stem().string() + "_bg_masks")).string();
}

std::string bg_remove_hires_dir(const std::string& video_path) {
    fs::path p(video_path);
    return (p.parent_path() / (p.stem().string() + "_bg_hires")).string();
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
    return app_models_dir() + "/u2net_human_seg.onnx";
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
    // Probe fps
    float fps = 30.f;
    {
        std::string probe = "ffprobe -v error -select_streams v:0"
                            " -show_entries stream=r_frame_rate"
                            " -of csv=p=0 \"" + input_path + "\" 2>/dev/null";
        FILE* p = popen(probe.c_str(), "r");
        if (p) {
            char buf[64] = {};
            if (fgets(buf, sizeof(buf), p)) {
                int nr, dr;
                if (sscanf(buf, "%d/%d", &nr, &dr) == 2 && dr > 0)
                    fps = (float)nr / dr;
                else
                    fps = (float)atof(buf);
            }
            pclose(p);
        }
    }
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
    opts.SetIntraOpNumThreads((int)std::thread::hardware_concurrency());
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
        std::string ffcmd = "ffmpeg -hide_banner -loglevel error"
                            " -i \"" + input_path + "\"";
        if (num_frames > 0 && start_frame >= 0) {
            int ef = start_frame + num_frames - 1;
            ffcmd += " -vf \"select='between(n," + std::to_string(start_frame)
                     + "," + std::to_string(ef) + ")'\" -vsync vfr";
        } else if (start_time > 0.001f) {
            char ss[32]; snprintf(ss, sizeof(ss), " -ss %.6f", start_time);
            ffcmd += ss;
        }
        ffcmd += " -f image2 \"" + tmpdir + "/%06d.jpg\" 2>/dev/null";
        system(ffcmd.c_str());  // NOLINT
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

    std::vector<float> input_data(3 * U2NET_SIZE * U2NET_SIZE);
    int64_t shape[] = {1, 3, U2NET_SIZE, U2NET_SIZE};
    auto mem_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    for (int i = 0; i < total; i++) {
        int w, h, ch;
        uint8_t* img = stbi_load(frames[i].string().c_str(), &w, &h, &ch, 3);
        if (!img) {
            data->progress.store((float)(i+1) / total);
            continue;
        }

        bilinear_resize_rgb(img, w, h, input_data.data(), U2NET_SIZE, U2NET_SIZE);
        stbi_image_free(img);

        Ort::Value in_tensor = Ort::Value::CreateTensor<float>(
            mem_info, input_data.data(), input_data.size(), shape, 4);

        std::vector<Ort::Value> outputs;
        try {
            outputs = session->Run(Ort::RunOptions{nullptr},
                                   in_names, &in_tensor, 1,
                                   out_names, 1);
        } catch (...) {
            data->progress.store((float)(i+1) / total);
            continue;
        }

        float* mask = outputs[0].GetTensorMutableData<float>();
        auto mshape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
        int mh = (int)mshape[2], mw = (int)mshape[3];

        std::vector<uint8_t> alpha(w * h);
        bilinear_resize_mask(mask, mw, mh, alpha.data(), w, h);
        gaussian_blur_mask(alpha.data(), w, h);

        JpegBuf buf;
        stbi_write_jpg_to_func(JpegBuf::cb, &buf, w, h, 1, alpha.data(), 90);
        fwrite(buf.data.data(), 1, buf.data.size(), mjpeg_f);
        fflush(mjpeg_f);

        data->progress.store((float)(i+1) / total);
    }

    fclose(mjpeg_f);
    // Remove stale seek-table so it gets rebuilt on next read
    fs::remove(output_dir + "/bg_masks.idx");
    fs::remove_all(tmpdir);
    data->status.store(1);
}

// ── Public API ────────────────────────────────────────────────────────────────

void bg_remove_start(AppState& state, int track_idx, int clip_idx) {
    if (track_idx < 0 || track_idx >= (int)state.tracks.size()) return;
    auto& track = state.tracks[track_idx];
    if (clip_idx < 0 || clip_idx >= (int)track.clips.size()) return;
    Clip& clip = track.clips[clip_idx];
    if (clip.clip_type != ClipType::Video || clip.text.empty()) return;

    std::string mjpeg = proxy_mjpeg_path(clip.text);
    if (!fs::exists(mjpeg)) {
        clip.bg_remove_status = BgRemoveStatus::Error;
        clip.bg_remove_error  = "Proxy not ready — wait for proxy generation to finish.";
        return;
    }

    std::string mdir = bg_remove_proxy_dir(clip.text);
    clip.bg_remove_mask_dir = mdir;
    clip.bg_remove_status   = BgRemoveStatus::Processing;
    clip.bg_remove_progress = 0.f;
    clip.bg_remove_error.clear();

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

    float fps = 30.f;
    {
        std::string probe = "ffprobe -v error -select_streams v:0"
                            " -show_entries stream=r_frame_rate"
                            " -of csv=p=0 \"" + video_path + "\" 2>/dev/null";
        FILE* p = popen(probe.c_str(), "r");
        if (p) {
            char buf[64] = {};
            if (fgets(buf, sizeof(buf), p)) {
                int nr, dr;
                if (sscanf(buf, "%d/%d", &nr, &dr) == 2 && dr > 0)
                    fps = (float)nr / dr;
                else
                    fps = (float)atof(buf);
            }
            pclose(p);
        }
    }
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
