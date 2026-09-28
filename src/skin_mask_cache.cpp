#include "skin_mask_cache.h"

#include "platform.h"
#include "skin_segment.h"

#if PMS_HAS_GL
#include "gl_compat.h"
#endif

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <list>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

// ── Job queue (any thread → worker) ──────────────────────────────────────────

namespace {

struct FillJob {
    std::string key;
    std::vector<uint8_t> rgb;   // source frame, RGB
    int sw = 0, sh = 0;         // source size
    int fw = 0, fh = 0;         // display size to cache at
};

struct ReadyMask {
    std::string key;
    std::vector<uint8_t> bytes;  // fw*fh skin confidence 0..255
    int fw = 0, fh = 0;
};

std::mutex g_mtx;
std::deque<FillJob> g_jobs;
std::vector<ReadyMask> g_ready;  // pumped to GL by skin_mask_pump
bool g_worker_up = false;

// Keys with a job queued/in-flight or a ready mask awaiting pump. Checked in
// skin_mask_request (skip duplicates) and skin_mask_texture (don't report a
// miss as fillable twice). Cleared when the mask lands in the GL cache.
std::unordered_map<std::string, bool> g_pending;

void worker_main() {
    for (;;) {
        FillJob job;
        {
            std::unique_lock<std::mutex> lk(g_mtx);
            // Poll-wait: condition_variable in a detached never-joined worker
            // is fine, but a 100 ms poll keeps shutdown trivial (no wakeups
            // needed) and matches bg_remove's cadence.
            if (g_jobs.empty()) {
                lk.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            job = std::move(g_jobs.front());
            g_jobs.pop_front();
        }
        // Source-size confidences, then bilinear downsample to display size.
        std::vector<float> conf;
        std::string err;
        if (!skin_segment_run(job.rgb.data(), job.sw, job.sh, conf, &err)) {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_pending.erase(job.key);
            continue;
        }
        ReadyMask rm;
        rm.key = job.key;
        rm.fw = job.fw;
        rm.fh = job.fh;
        rm.bytes.resize((size_t)job.fw * job.fh);
        for (int y = 0; y < job.fh; ++y) {
            float sy = (y + 0.5f) * job.sh / job.fh - 0.5f;
            int y0 = (int)floorf(sy);
            float fy = sy - y0;
            if (y0 < 0) { y0 = 0; fy = 0.f; }
            if (y0 >= job.sh - 1) { y0 = job.sh - 1; fy = 0.f; }
            for (int x = 0; x < job.fw; ++x) {
                float sx = (x + 0.5f) * job.sw / job.fw - 0.5f;
                int x0 = (int)floorf(sx);
                float fx = sx - x0;
                if (x0 < 0) { x0 = 0; fx = 0.f; }
                if (x0 >= job.sw - 1) { x0 = job.sw - 1; fx = 0.f; }
                const float* p00 = &conf[((size_t)y0 * job.sw + x0) * SKIN_NCLASSES];
                const float* p10 = p00 + SKIN_NCLASSES;
                const float* p01 = p00 + (size_t)job.sw * SKIN_NCLASSES;
                const float* p11 = p01 + SKIN_NCLASSES;
                float v = ((skin_mask_conf(p00) * (1 - fx) + skin_mask_conf(p10) * fx) * (1 - fy) +
                           (skin_mask_conf(p01) * (1 - fx) + skin_mask_conf(p11) * fx) * fy);
                v = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
                rm.bytes[(size_t)y * job.fw + x] = (uint8_t)(v * 255.f + 0.5f);
            }
        }
        std::lock_guard<std::mutex> lk(g_mtx);
        g_ready.push_back(std::move(rm));
    }
}

void ensure_worker() {
    if (g_worker_up) return;
    g_worker_up = true;
    std::thread(worker_main).detach();
}

#if PMS_HAS_GL
// ── GL texture cache (GL thread only) ────────────────────────────────────────
// Exact-size entries: (key, w, h) → texture. A size change is a miss (the
// worker re-fills at the new size); stale sizes are evicted lazily.

struct TexEntry {
    unsigned tex = 0;
    int w = 0, h = 0;
    std::list<std::string>::iterator lru;
};

std::unordered_map<std::string, TexEntry> g_tex;  // key = src_key@wxh
std::list<std::string> g_lru;
static constexpr int k_tex_max = 12;

std::string tex_key(const std::string& src_key, int w, int h) {
    return src_key + "@" + std::to_string(w) + "x" + std::to_string(h);
}
#endif  // PMS_HAS_GL

}  // namespace

bool skin_mask_available() { return skin_segment_available(); }

unsigned skin_mask_texture(const std::string& src_key, int w, int h) {
#if !PMS_HAS_GL
    (void)src_key; (void)w; (void)h;
    return 0;
#else
    if (!skin_segment_available() || src_key.empty() || w <= 0 || h <= 0)
        return 0;
    auto it = g_tex.find(tex_key(src_key, w, h));
    if (it == g_tex.end()) return 0;
    g_lru.erase(it->second.lru);
    g_lru.push_front(it->first);
    it->second.lru = g_lru.begin();
    return it->second.tex;
#endif
}

void skin_mask_request(const std::string& src_key,
                       const uint8_t* rgb, int w, int h,
                       int frame_w, int frame_h) {
    if (!skin_segment_available() || src_key.empty() || !rgb ||
        w <= 0 || h <= 0 || frame_w <= 0 || frame_h <= 0)
        return;
    // Cap the ORT input: 256x256 net, so downsample large sources on the
    // caller thread (cheap box average) — the worker's bilinear 256 resize
    // runs per pixel and a 1920x1440 source would cost ~40 ms before ORT.
    const uint8_t* in = rgb;
    int iw = w, ih = h;
    std::vector<uint8_t> owned;
    if ((int64_t)w * h > 640 * 480) {
        iw = 640;
        ih = h * 640 / w;
        if (ih < 1) ih = 1;
        owned.resize((size_t)iw * ih * 3);
        for (int y = 0; y < ih; ++y) {
            int y0 = y * h / ih, y1 = (y + 1) * h / ih;
            if (y1 <= y0) y1 = y0 + 1;
            for (int x = 0; x < iw; ++x) {
                int x0 = x * w / iw, x1 = (x + 1) * w / iw;
                if (x1 <= x0) x1 = x0 + 1;
                int acc[3] = {0, 0, 0};
                for (int yy = y0; yy < y1; ++yy)
                    for (int xx = x0; xx < x1; ++xx) {
                        const uint8_t* p = rgb + ((size_t)yy * w + xx) * 3;
                        acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2];
                    }
                int n = (y1 - y0) * (x1 - x0);
                uint8_t* d = &owned[((size_t)y * iw + x) * 3];
                d[0] = (uint8_t)(acc[0] / n); d[1] = (uint8_t)(acc[1] / n);
                d[2] = (uint8_t)(acc[2] / n);
            }
        }
        in = owned.data();
    }
    std::lock_guard<std::mutex> lk(g_mtx);
#if PMS_HAS_GL
    if (g_tex.find(tex_key(src_key, frame_w, frame_h)) != g_tex.end())
        return;  // already cached
#endif
    if (g_pending.count(src_key)) return;  // queued / in-flight / ready
    if (g_jobs.size() > 8) return;         // scrub storm: drop, retry next frame
    FillJob job;
    job.key = src_key;
    job.rgb.assign(in, in + (size_t)iw * ih * 3);
    job.sw = iw; job.sh = ih;
    job.fw = frame_w; job.fh = frame_h;
    g_jobs.push_back(std::move(job));
    g_pending[src_key] = true;
    ensure_worker();
}

void skin_mask_pump(int max_uploads) {
#if !PMS_HAS_GL
    (void)max_uploads;
    return;
#else
    if (!skin_segment_available()) return;
    std::vector<ReadyMask> done;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        int n = std::min<int>(max_uploads, (int)g_ready.size());
        for (int i = 0; i < n; ++i) {
            done.push_back(std::move(g_ready.back()));
            g_ready.pop_back();
        }
    }
    for (auto& rm : done) {
        unsigned tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, rm.fw, rm.fh, 0,
                     GL_RED, GL_UNSIGNED_BYTE, rm.bytes.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        std::string k = tex_key(rm.key, rm.fw, rm.fh);
        if ((int)g_tex.size() >= k_tex_max && !g_lru.empty()) {
            auto ev = g_tex.find(g_lru.back());
            if (ev != g_tex.end()) {
                glDeleteTextures(1, &ev->second.tex);
                g_tex.erase(ev);
            }
            g_lru.pop_back();
        }
        g_lru.push_front(k);
        g_tex[k] = {tex, rm.fw, rm.fh, g_lru.begin()};
        std::lock_guard<std::mutex> lk(g_mtx);
        g_pending.erase(rm.key);
    }
#endif
}

void skin_mask_evict_all() {
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_jobs.clear();
        g_ready.clear();
        g_pending.clear();
    }
#if PMS_HAS_GL
    for (auto& kv : g_tex) glDeleteTextures(1, &kv.second.tex);
    g_tex.clear();
    g_lru.clear();
#endif
}

int skin_mask_pending() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return (int)g_jobs.size() + (int)g_ready.size();
}
