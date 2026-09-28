#include "skin_segment.h"

#include "paths.h"


#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <mutex>

#include "stb_image.h"
#include "stb_image_write.h"

namespace fs = std::filesystem;

static std::string model_path() {
    return (fs::path(app_models_dir()) / "selfie_multiclass_256x256.onnx").string();
}

bool skin_segment_available() { return fs::exists(model_path()); }

namespace {

Ort::Env& seg_env() {
    static Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "pms-skin");
    return env;
}

std::mutex g_sess_mtx;
std::unique_ptr<Ort::Session> g_sess;
std::string g_sess_path;

bool ensure_session(std::string* err) {
    std::lock_guard<std::mutex> lk(g_sess_mtx);
    std::string want = model_path();
    if (g_sess && g_sess_path == want) return true;
    g_sess.reset();
    Ort::SessionOptions opts;
    opts.SetIntraOpNumThreads(2);
    opts.SetInterOpNumThreads(1);
    opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    try {
        g_sess = std::make_unique<Ort::Session>(seg_env(), want.c_str(), opts);
    } catch (const Ort::Exception& e) {
        if (err) *err = std::string("skin model load failed: ") + e.what();
        return false;
    }
    g_sess_path = want;
    return true;
}

// Bilinear RGB resize, 0..255 u8 in → 0..1 float out, 256x256.
void resize_rgb_to_256(const uint8_t* src, int sw, int sh, float* dst) {
    for (int y = 0; y < 256; ++y) {
        float sy = (y + 0.5f) * sh / 256.f - 0.5f;
        int y0 = (int)floorf(sy);
        float fy = sy - y0;
        if (y0 < 0) { y0 = 0; fy = 0.f; }
        if (y0 >= sh - 1) { y0 = sh - 1; fy = 0.f; }
        for (int x = 0; x < 256; ++x) {
            float sx = (x + 0.5f) * sw / 256.f - 0.5f;
            int x0 = (int)floorf(sx);
            float fx = sx - x0;
            if (x0 < 0) { x0 = 0; fx = 0.f; }
            if (x0 >= sw - 1) { x0 = sw - 1; fx = 0.f; }
            const uint8_t* p00 = src + ((size_t)y0 * sw + x0) * 3;
            const uint8_t* p10 = p00 + 3;
            const uint8_t* p01 = p00 + (size_t)sw * 3;
            const uint8_t* p11 = p01 + 3;
            float* d = dst + ((size_t)y * 256 + x) * 3;
            for (int c = 0; c < 3; ++c) {
                float v = (p00[c] * (1 - fx) + p10[c] * fx) * (1 - fy) +
                          (p01[c] * (1 - fx) + p11[c] * fx) * fy;
                d[c] = v / 255.f;
            }
        }
    }
}

// Bilinear upsample of one 256x256 class plane (with softmax applied from
// the shared logits) into a w*h float plane.
void upsample_softmax_plane(const float* logits, int cls, float* plane, int w, int h) {
    for (int y = 0; y < h; ++y) {
        float sy = (y + 0.5f) * 256.f / h - 0.5f;
        int y0 = (int)floorf(sy);
        float fy = sy - y0;
        if (y0 < 0) { y0 = 0; fy = 0.f; }
        if (y0 >= 255) { y0 = 255; fy = 0.f; }
        for (int x = 0; x < w; ++x) {
            float sx = (x + 0.5f) * 256.f / w - 0.5f;
            int x0 = (int)floorf(sx);
            float fx = sx - x0;
            if (x0 < 0) { x0 = 0; fx = 0.f; }
            if (x0 >= 255) { x0 = 255; fx = 0.f; }
            // Softmax at the 4 surrounding logits, then bilinear blend of
            // this class's confidence (equivalent to blending logits only
            // when the field is locally flat; keeps edges honest).
            float c00, c10, c01, c11;
            const float* q[4] = {logits + ((size_t)y0 * 256 + x0) * 6,
                                 logits + ((size_t)y0 * 256 + x0 + 1) * 6,
                                 logits + ((size_t)(y0 + 1) * 256 + x0) * 6,
                                 logits + ((size_t)(y0 + 1) * 256 + x0 + 1) * 6};
            float* cs[4] = {&c00, &c10, &c01, &c11};
            for (int qk = 0; qk < 4; ++qk) {
                float mx = q[qk][0];
                for (int k = 1; k < 6; ++k) mx = std::max(mx, q[qk][k]);
                float s = 0.f;
                for (int k = 0; k < 6; ++k) s += expf(q[qk][k] - mx);
                *cs[qk] = expf(q[qk][cls] - mx) / s;
            }
            plane[(size_t)y * w + x] =
                (c00 * (1 - fx) + c10 * fx) * (1 - fy) + (c01 * (1 - fx) + c11 * fx) * fy;
        }
    }
}

bool run_logits(const uint8_t* rgb, int w, int h, std::vector<float>& logits,
                std::string* err) {
    if (!skin_segment_available()) {
        if (err) *err = "skin model not found: " + model_path();
        return false;
    }
    if (!ensure_session(err)) return false;
    static thread_local std::vector<float> input;
    input.resize((size_t)256 * 256 * 3);
    resize_rgb_to_256(rgb, w, h, input.data());

    std::lock_guard<std::mutex> lk(g_sess_mtx);
    Ort::MemoryInfo mem =
        Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    int64_t shape[4] = {1, 256, 256, 3};
    Ort::Value in = Ort::Value::CreateTensor<float>(mem, input.data(),
                                                    input.size(), shape, 4);
    const char* in_names[] = {"input_29"};
    const char* out_names[] = {"Identity"};
    std::vector<Ort::Value> outs;
    try {
        outs = g_sess->Run(Ort::RunOptions{nullptr}, in_names, &in, 1,
                           out_names, 1);
    } catch (const Ort::Exception& e) {
        if (err) *err = std::string("skin inference failed: ") + e.what();
        return false;
    }
    const float* y = outs[0].GetTensorData<float>();
    logits.assign(y, y + (size_t)256 * 256 * 6);
    return true;
}

}  // namespace

bool skin_segment_run(const uint8_t* rgb, int w, int h,
                      std::vector<float>& conf, std::string* err) {
    if (!rgb || w <= 0 || h <= 0) {
        if (err) *err = "bad image";
        return false;
    }
    std::vector<float> logits;
    if (!run_logits(rgb, w, h, logits, err)) return false;
    conf.resize((size_t)w * h * SKIN_NCLASSES);
    std::vector<float> plane((size_t)w * h);
    for (int k = 0; k < SKIN_NCLASSES; ++k) {
        upsample_softmax_plane(logits.data(), k, plane.data(), w, h);
        for (int i = 0; i < w * h; ++i)
            conf[(size_t)i * SKIN_NCLASSES + k] = plane[i];
    }
    return true;
}

bool skin_segment_image(const std::string& image_path, const std::string& out_dir,
                        std::array<std::string, SKIN_NCLASSES>& out_paths,
                        int& out_w, int& out_h,
                        const std::function<void(float)>& progress,
                        std::string* err) {
    if (!skin_segment_available()) {
        if (err) *err = "skin model not found: " + model_path();
        return false;
    }
    int w = 0, h = 0, ch = 0;
    uint8_t* px = stbi_load(image_path.c_str(), &w, &h, &ch, 3);
    if (!px) {
        if (err) *err = "cannot decode image: " + image_path;
        return false;
    }
    // Masks are written at the decoded image's own resolution (the caller's
    // display size). Rotation-aware stills (EXIF transpose) decode upright
    // via stbi, so w/h already are the display dims.
    const int dw = w, dh = h;
    std::vector<float> logits;
    bool ok = run_logits(px, w, h, logits, err);
    stbi_image_free(px);
    if (!ok) return false;
    if (progress) progress(0.5f);

    std::error_code ec;
    fs::create_directories(out_dir, ec);
    if (ec) {
        if (err) *err = "cannot create out_dir: " + out_dir;
        return false;
    }
    // Upsample each class plane to DISPLAY size, quantise to 8-bit, write.
    std::vector<float> plane((size_t)dw * dh);
    std::vector<uint8_t> bytes((size_t)dw * dh);
    for (int k = 0; k < SKIN_NCLASSES; ++k) {
        upsample_softmax_plane(logits.data(), k, plane.data(), dw, dh);
        for (int i = 0; i < dw * dh; ++i) {
            float v = plane[i];
            v = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
            bytes[i] = (uint8_t)(v * 255.f + 0.5f);
        }
        std::string dst =
            (fs::path(out_dir) / (std::string(SKIN_CLASS_NAMES[k]) + ".png")).string();
        if (!stbi_write_png(dst.c_str(), dw, dh, 1, bytes.data(), dw)) {
            if (err) *err = "cannot write mask: " + dst;
            return false;
        }
        out_paths[k] = dst;
        if (progress) progress(0.5f + 0.5f * (k + 1) / SKIN_NCLASSES);
    }
    out_w = dw;
    out_h = dh;
    return true;
}
