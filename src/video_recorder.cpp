#include "video_recorder.h"
#include "audio.h"
#include "history.h"
#include "globals.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

// Camera→screen→performer latency: frames are captured this long before the
// loop position they correspond to. Webcams typically run 2–4 frames behind;
// 100 ms is a sane default until per-device calibration exists.
static constexpr float kCamLatency = 0.100f;

struct VFrame {
    float                t = 0.f;   // loop-stream time (seconds since loop start 0)
    std::vector<uint8_t> jpeg;
};

static bool   s_active = false;
static bool   s_test_pattern = false;
static int    s_ti = -1, s_ci = -1;
static float  s_lp_start = 0.f, s_lp_end = 0.f;
static float  s_loop_len = 0.f;
static int    s_take_count = 0;
static FILE*  s_cap = nullptr;            // capture child stdout (MJPEG stream)
static std::vector<uint8_t> s_raw;        // undecoded byte tail from the pipe
static std::vector<VFrame>  s_frames;     // frames of the take in progress
static std::vector<uint8_t> s_last_jpeg;  // most recent frame (live preview)
static std::chrono::steady_clock::time_point s_t0;  // capture stream origin

// ── Helpers ───────────────────────────────────────────────────────────────────

static Clip* target_brick(AppState& state) {
    auto match = [&](int ti, int ci) -> Clip* {
        if (ti < 0 || ti >= (int)state.tracks.size()) return nullptr;
        auto& clips = state.tracks[ti].clips;
        if (ci < 0 || ci >= (int)clips.size()) return nullptr;
        Clip* cl = &clips[ci];
        if (cl->clip_type != ClipType::VideoRecord) return nullptr;
        if (s_active && (fabsf(cl->start - s_lp_start) > 1e-4f ||
                         fabsf(cl->end   - s_lp_end)   > 1e-4f)) return nullptr;
        return cl;
    };
    if (Clip* cl = match(s_ti, s_ci)) return cl;
    if (!s_active) return nullptr;
    for (int ti = 0; ti < (int)state.tracks.size(); ++ti)
        for (int ci = 0; ci < (int)state.tracks[ti].clips.size(); ++ci)
            if (Clip* cl = match(ti, ci)) { s_ti = ti; s_ci = ci; return cl; }
    return nullptr;
}

static std::string takes_dir() {
    std::string dir = g_managed_dir + "/takes";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

static std::string next_take_path() {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    long ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    char buf[160];
    snprintf(buf, sizeof(buf), "%s/vtake_%ld_%02d.avi",
             takes_dir().c_str(), ms, s_take_count + 1);
    return buf;
}

// JPEG dimensions from the SOF0/SOF2 marker (needed for the AVI headers).
static bool jpeg_dims(const std::vector<uint8_t>& j, int& w, int& h) {
    for (size_t i = 2; i + 9 < j.size(); ) {
        if (j[i] != 0xFF) { ++i; continue; }
        uint8_t m = j[i+1];
        if (m == 0xC0 || m == 0xC1 || m == 0xC2) {
            h = (j[i+5] << 8) | j[i+6];
            w = (j[i+7] << 8) | j[i+8];
            return w > 0 && h > 0;
        }
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD9)) { i += 2; continue; }
        i += 2 + ((j[i+2] << 8) | j[i+3]);   // skip marker segment
    }
    return false;
}

// Hand-rolled MJPEG-in-AVI muxer. ffmpeg's mjpeg demuxer mishandles
// non-integer -framerate values (measured: 32.25 fps in produces a file a
// third of the right length), and the AVI stream header's scale/rate pair
// holds our exact rational anyway: rate = nframes·1000, scale = loop_ms.
static bool write_take_avi(const std::string& path,
                            const std::vector<VFrame>& frames, float dur_s) {
    int w = 0, h = 0;
    if (!jpeg_dims(frames[0].jpeg, w, h)) return false;
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;

    uint32_t n = (uint32_t)frames.size();
    uint32_t rate  = n * 1000u;
    uint32_t scale = (uint32_t)lroundf(dur_s * 1000.f);
    uint32_t usec_pf = (uint32_t)llroundf(dur_s * 1e6f / (float)n);

    auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    auto tag = [&](const char* t) { fwrite(t, 4, 1, f); };

    uint32_t movi_body = 0;                  // sum of '00dc' chunks (padded)
    std::vector<uint32_t> sizes(n);
    for (uint32_t i = 0; i < n; ++i) {
        sizes[i] = (uint32_t)frames[i].jpeg.size();
        movi_body += 8 + sizes[i] + (sizes[i] & 1);
    }
    uint32_t hdrl_sz = 4 + (8+56) + (8 + 4 + (8+56) + (8+40));
    uint32_t movi_sz = 4 + movi_body;
    uint32_t idx_sz  = n * 16;
    uint32_t riff_sz = 4 + (8 + hdrl_sz) + (8 + movi_sz) + (8 + idx_sz);

    tag("RIFF"); u32(riff_sz); tag("AVI ");
    tag("LIST"); u32(hdrl_sz); tag("hdrl");
    tag("avih"); u32(56);
    u32(usec_pf); u32(0); u32(0); u32(0x10 /*HASINDEX*/);
    u32(n); u32(0); u32(1); u32(0);
    u32((uint32_t)w); u32((uint32_t)h);
    u32(0); u32(0); u32(0); u32(0);
    tag("LIST"); u32(4 + (8+56) + (8+40)); tag("strl");
    tag("strh"); u32(56);
    tag("vids"); tag("MJPG");
    u32(0); u16(0); u16(0); u32(0);
    u32(scale); u32(rate);
    u32(0); u32(n); u32(0); u32((uint32_t)-1); u32(0);
    u16(0); u16(0); u16((uint16_t)w); u16((uint16_t)h);
    tag("strf"); u32(40);
    u32(40); u32((uint32_t)w); u32((uint32_t)h);
    u16(1); u16(24); tag("MJPG");
    u32((uint32_t)(w * h * 3)); u32(0); u32(0); u32(0); u32(0);
    tag("LIST"); u32(movi_sz); tag("movi");
    std::vector<uint32_t> offs(n);
    uint32_t off = 4;                        // relative to 'movi' tag start
    for (uint32_t i = 0; i < n; ++i) {
        offs[i] = off;
        tag("00dc"); u32(sizes[i]);
        fwrite(frames[i].jpeg.data(), 1, sizes[i], f);
        if (sizes[i] & 1) fputc(0, f);
        off += 8 + sizes[i] + (sizes[i] & 1);
    }
    tag("idx1"); u32(idx_sz);
    for (uint32_t i = 0; i < n; ++i) {
        tag("00dc"); u32(0x10 /*KEYFRAME*/); u32(offs[i]); u32(sizes[i]);
    }
    bool ok = !ferror(f);
    fclose(f);
    if (!ok) ::unlink(path.c_str());
    return ok;
}

// Write the take with duration == loop length exactly.
static bool finalize_take(AppState& state) {
    if (s_frames.size() < 2 || s_loop_len <= 0.f) { s_frames.clear(); return true; }
    std::string path = next_take_path();
    bool ok = write_take_avi(path, s_frames, s_loop_len);
    s_frames.clear();
    if (!ok) { ::unlink(path.c_str()); return false; }

    Clip* cl = target_brick(state);
    if (!cl) { ::unlink(path.c_str()); return false; }
    cl->rec_takes.push_back(path);
    cl->rec_take_sel = (int)cl->rec_takes.size() - 1;
    cl->text = path;   // mirrored: video draw/export paths read clip.text
    state.proxy_scan_needed = true;   // proxy + slot for canvas preview
    ++s_take_count;
    return true;
}

// ── Capture child ─────────────────────────────────────────────────────────────

static bool capture_start() {
    s_test_pattern = !fs::exists("/dev/video0");
    const char* cmd =
        s_test_pattern
        // Camera-less dev fallback: timestamped test pattern at webcam-ish
        // specs. -re paces lavfi to real time (a camera self-paces).
        ? "ffmpeg -hide_banner -loglevel error"
          " -re -f lavfi -i testsrc2=size=1280x720:rate=30"
          " -c:v mjpeg -q:v 4 -f mjpeg pipe:1 2>/dev/null"
        : "ffmpeg -hide_banner -loglevel error"
          " -f v4l2 -input_format mjpeg -framerate 30 -video_size 1280x720"
          " -i /dev/video0 -c:v copy -f mjpeg pipe:1 2>/dev/null";
    s_cap = popen(cmd, "r");
    if (!s_cap) return false;
    int fd = fileno(s_cap);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return true;
}

static void capture_stop() {
    if (s_cap) { pclose(s_cap); s_cap = nullptr; }
    s_raw.clear();
    s_raw.shrink_to_fit();
}

// Drain the pipe and split the MJPEG byte stream into frames on JPEG
// SOI (FFD8) / EOI (FFD9) markers. Each completed frame is stamped with the
// current loop-stream time.
static void capture_drain() {
    if (!s_cap) return;
    uint8_t buf[1 << 16];
    ssize_t n;
    int fd = fileno(s_cap);
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        s_raw.insert(s_raw.end(), buf, buf + n);

    float now_t = std::chrono::duration<float>(
                      std::chrono::steady_clock::now() - s_t0).count();

    size_t pos = 0;
    while (true) {
        // find SOI
        size_t soi = pos;
        while (soi + 1 < s_raw.size() &&
               !(s_raw[soi] == 0xFF && s_raw[soi+1] == 0xD8)) ++soi;
        if (soi + 1 >= s_raw.size()) break;
        // find EOI after it
        size_t eoi = soi + 2;
        while (eoi + 1 < s_raw.size() &&
               !(s_raw[eoi] == 0xFF && s_raw[eoi+1] == 0xD9)) ++eoi;
        if (eoi + 1 >= s_raw.size()) break;

        VFrame f;
        f.t = now_t - kCamLatency;
        f.jpeg.assign(s_raw.begin() + (long)soi, s_raw.begin() + (long)eoi + 2);
        s_last_jpeg = f.jpeg;
        s_frames.push_back(std::move(f));
        pos = eoi + 2;
    }
    if (pos > 0) s_raw.erase(s_raw.begin(), s_raw.begin() + (long)pos);
}

// ── Public API ────────────────────────────────────────────────────────────────

bool vrecorder_active() { return s_active; }
bool vrecorder_is_target(int ti, int ci) { return s_active && ti == s_ti && ci == s_ci; }
int  vrecorder_take_count() { return s_take_count; }
bool vrecorder_using_test_pattern() { return s_active && s_test_pattern; }

bool vrecorder_latest_jpeg(std::vector<uint8_t>& out) {
    if (s_last_jpeg.empty()) return false;
    out = s_last_jpeg;
    return true;
}

bool vrecorder_start(AppState& state, int ti, int ci) {
    if (s_active) return false;
    s_ti = ti; s_ci = ci;
    Clip* cl = target_brick(state);
    if (!cl || cl->end - cl->start < 0.25f) { s_ti = s_ci = -1; return false; }
    s_lp_start = cl->start;
    s_lp_end   = cl->end;
    s_loop_len = s_lp_end - s_lp_start;

    if (!capture_start()) { s_ti = s_ci = -1; return false; }

    // Wait for the camera to actually deliver a frame (device/child spin-up
    // can take seconds); everything before the transport starts is discarded
    // so the first take isn't padded with pre-roll. Mirrors the audio
    // recorder's capture-drain spin.
    {
        s_t0 = std::chrono::steady_clock::now();   // provisional origin
        bool got = false;
        for (int i = 0; i < 100 && !got; ++i) {    // up to ~5 s
            capture_drain();
            got = !s_frames.empty() || !s_last_jpeg.empty();
            if (!got) usleep(50 * 1000);
        }
        s_frames.clear();
        if (!got) {
            capture_stop();
            s_ti = s_ci = -1;
            return false;
        }
    }

    // Join the loop transport if an audio Record brick already drives it on
    // the same bounds (co-recording video + audio); otherwise start it.
    audio_init();
    bool join = state.playing && audio_loop_active();
    if (!join) {
        audio_set_loop(s_lp_start, s_lp_end);
        audio_seek(s_lp_start);
        state.playhead        = s_lp_start;
        state.playing         = true;
        state.play_start_pos  = s_lp_start;
        state.play_start_wall = std::chrono::steady_clock::now();
        audio_play();
    }
    // Loop-stream origin: "now" corresponds to the current loop position, so
    // time 0 of the capture stream = that much before the current instant.
    float cur = state.playhead - s_lp_start;
    if (join)
        cur = fmodf(fmaxf(0.f,
                  std::chrono::duration<float>(std::chrono::steady_clock::now()
                      - state.play_start_wall).count()
                  + state.play_start_pos - s_lp_start),
              fmaxf(s_loop_len, 1e-3f));
    else
        cur = 0.f;
    s_t0 = std::chrono::steady_clock::now() - std::chrono::microseconds(
               (long)(cur * 1e6f));

    s_frames.clear();
    s_last_jpeg.clear();
    s_take_count = 0;
    s_active     = true;
    return true;
}

void vrecorder_stop(AppState& state, bool keep_partial) {
    if (!s_active) return;
    capture_drain();
    capture_stop();

    // Keep a partial last pass when it has at least half a second in it.
    if (keep_partial && !s_frames.empty()) {
        float span = s_frames.back().t - s_frames.front().t;
        if (span > 0.5f) {
            // Mux at the natural rate of the partial (duration = real span).
            float keep_len = s_loop_len;
            s_loop_len = fmaxf(span, 0.5f);
            finalize_take(state);
            s_loop_len = keep_len;
        }
    }
    s_frames.clear();

    if (s_take_count > 0)
        history_push(state, "Record " + std::to_string(s_take_count) +
                            " video take(s)");

    // Only tear the transport down if the audio recorder isn't still using it.
    extern bool recorder_active();
    if (!recorder_active()) {
        audio_clear_loop();
        state.playing = false;
        audio_pause();
    }

    s_active = false;
    s_ti = s_ci = -1;
    s_last_jpeg.clear();
}

void vrecorder_tick(AppState& state) {
    if (!s_active) return;

    if (!target_brick(state) || !state.playing || !audio_loop_active()) {
        bool keep = target_brick(state) != nullptr;
        vrecorder_stop(state, keep);
        return;
    }

    capture_drain();

    // Every frame past (take_count+1)·loop_len closes a take. Frames are
    // bucketed by their stamped loop-stream time, so a slow camera can't
    // smear takes across boundaries.
    while (!s_frames.empty()) {
        float boundary = (float)(s_take_count + 1) * s_loop_len;
        // Partition: frames with t < boundary belong to the current take.
        size_t split = 0;
        while (split < s_frames.size() && s_frames[split].t < boundary) ++split;
        if (split >= s_frames.size()) break;   // boundary not reached yet

        std::vector<VFrame> rest(s_frames.begin() + (long)split, s_frames.end());
        s_frames.resize(split);
        if (!finalize_take(state)) {
            vrecorder_stop(state, false);
            return;
        }
        s_frames = std::move(rest);
    }
}
