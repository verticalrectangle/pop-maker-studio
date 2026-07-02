#include "face_cache.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// ── In-memory registry ────────────────────────────────────────────────────────

namespace {

// Per-frame record: score + FT_NPTS*2 raw coords + FT_NBLEND blendshapes.
static constexpr size_t FC_REC = 1 + (size_t)FT_NPTS * 2 + FT_NBLEND;

struct CacheData {
    int   rot_q = 0;
    float fps   = 0.f;
    int   raw_w = 0, raw_h = 0;
    int   count = 0;
    std::vector<float> rec;     // count * FC_REC
};

struct Entry {
    FaceCacheStatus    status = FaceCacheStatus::None;
    float              progress = 0.f;
    int                want_rot_q = 0;
    std::shared_ptr<CacheData> data;     // Ready only
};

std::mutex                   g_mtx;
std::map<std::string, Entry> g_entries;
std::deque<std::string>      g_queue;
std::condition_variable      g_cv;
bool                         g_worker_up = false;
std::atomic<bool>            g_quit{false};

std::string sidecar(const std::string& take_path) { return take_path + ".face"; }

// nullptr unless the file exists, parses, and matches rot_q.
std::shared_ptr<CacheData> load_file(const std::string& take_path, int rot_q) {
    FILE* f = fopen(sidecar(take_path).c_str(), "rb");
    if (!f) return nullptr;
    uint32_t magic = 0, version = 0, count = 0;
    int32_t rq = 0, rw = 0, rh = 0;
    float fps = 0.f;
    bool hdr = fread(&magic, 4, 1, f) == 1 && fread(&version, 4, 1, f) == 1 &&
               fread(&rq, 4, 1, f) == 1 && fread(&fps, 4, 1, f) == 1 &&
               fread(&rw, 4, 1, f) == 1 && fread(&rh, 4, 1, f) == 1 &&
               fread(&count, 4, 1, f) == 1;
    // v3 = mesh + blendshapes; older caches (106-pt) are silently invalid and
    // rebuild on the next request.
    if (!hdr || magic != 0x46534D50 || version != 4 || rq != rot_q ||
        count == 0 || count > 1000000 || fps <= 0.f) {
        fclose(f);
        return nullptr;
    }
    auto d = std::make_shared<CacheData>();
    d->rot_q = rq; d->fps = fps; d->raw_w = rw; d->raw_h = rh;
    d->count = (int)count;
    d->rec.resize((size_t)count * FC_REC);
    bool ok = fread(d->rec.data(), sizeof(float), d->rec.size(), f) == d->rec.size();
    fclose(f);
    return ok ? d : nullptr;
}

void worker_main() {
    for (;;) {
        std::string path;
        int rot_q = 0;
        {
            std::unique_lock<std::mutex> lk(g_mtx);
            g_cv.wait(lk, [] { return !g_queue.empty() || g_quit.load(); });
            if (g_quit.load()) return;
            path = g_queue.front();
            g_queue.pop_front();
            rot_q = g_entries[path].want_rot_q;
            g_entries[path].status   = FaceCacheStatus::Building;
            g_entries[path].progress = 0.f;
        }
        bool ok = face_track_build_cache(
            path, rot_q, sidecar(path),
            [&path](float p) {
                std::lock_guard<std::mutex> lk(g_mtx);
                g_entries[path].progress = p;
            },
            &g_quit);
        std::lock_guard<std::mutex> lk(g_mtx);
        Entry& e = g_entries[path];
        if (ok && (e.data = load_file(path, rot_q))) {
            e.status = FaceCacheStatus::Ready;
        } else {
            e.status = FaceCacheStatus::Failed;
            e.data.reset();
        }
        g_cv.notify_all();              // wake ensure_sync waiters
    }
}

void ensure_worker() {
    if (!g_worker_up) {
        g_worker_up = true;
        std::thread(worker_main).detach();
    }
}

} // namespace

// ── Public API ────────────────────────────────────────────────────────────────

void face_cache_request(const std::string& take_path, int rot_q) {
    if (take_path.empty() || !face_track_available()) return;
    std::lock_guard<std::mutex> lk(g_mtx);
    Entry& e = g_entries[take_path];
    if (e.status == FaceCacheStatus::Building) return;
    if (e.status == FaceCacheStatus::Ready && e.data && e.data->rot_q == rot_q)
        return;
    if (e.status == FaceCacheStatus::Failed && e.want_rot_q == rot_q)
        return;                          // don't hammer a take that won't track
    // Try the sidecar before queueing a build (app restart with cache on disk).
    if (auto d = load_file(take_path, rot_q)) {
        e.status = FaceCacheStatus::Ready;
        e.data   = std::move(d);
        e.want_rot_q = rot_q;
        return;
    }
    e.status     = FaceCacheStatus::Building;   // queued counts as building
    e.progress   = 0.f;
    e.want_rot_q = rot_q;
    e.data.reset();
    g_queue.push_back(take_path);
    ensure_worker();
    g_cv.notify_one();
}

FaceCacheStatus face_cache_status(const std::string& take_path, float* progress) {
    std::lock_guard<std::mutex> lk(g_mtx);
    auto it = g_entries.find(take_path);
    if (it == g_entries.end()) {
        if (progress) *progress = 0.f;
        return FaceCacheStatus::None;
    }
    if (progress) *progress = it->second.progress;
    return it->second.status;
}

bool face_cache_obs(const std::string& take_path, int rot_q,
                    double src_t, FaceObs& out) {
    std::shared_ptr<CacheData> d;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        auto it = g_entries.find(take_path);
        if (it == g_entries.end() || it->second.status != FaceCacheStatus::Ready ||
            !it->second.data || it->second.data->rot_q != rot_q)
            return false;
        d = it->second.data;
    }
    int fi = (int)(src_t * d->fps + 0.5);
    if (fi < 0) fi = 0;
    if (fi >= d->count) fi = d->count - 1;
    const float* r = &d->rec[(size_t)fi * FC_REC];
    if (r[0] <= 0.f) return false;       // no face on this frame
    out.valid = true;
    out.score = r[0];
    out.w = d->raw_w; out.h = d->raw_h;
    for (int k = 0; k < FT_NPTS; ++k) {
        out.pts[k][0] = r[1 + k*2];
        out.pts[k][1] = r[2 + k*2];
    }
    const float* bl = r + 1 + FT_NPTS * 2;
    out.has_blend = false;
    for (int k = 0; k < FT_NBLEND; ++k) {
        out.blend[k] = bl[k];
        if (bl[k] != 0.f) out.has_blend = true;
    }
    return true;
}

bool face_cache_ensure_sync(const std::string& take_path, int rot_q,
                            const std::function<void(float)>& progress) {
    face_cache_request(take_path, rot_q);
    std::unique_lock<std::mutex> lk(g_mtx);
    for (;;) {
        Entry& e = g_entries[take_path];
        if (e.status == FaceCacheStatus::Ready && e.data &&
            e.data->rot_q == rot_q)
            return true;
        if (e.status == FaceCacheStatus::Failed) return false;
        if (progress) progress(e.progress);
        g_cv.wait_for(lk, std::chrono::milliseconds(200));
    }
}
