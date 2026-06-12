#include "recorder.h"
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

#include <unistd.h>

namespace fs = std::filesystem;

static bool   s_active     = false;
static int    s_ti = -1, s_ci = -1;
static float  s_lp_start = 0.f, s_lp_end = 0.f;  // identity check: brick must keep these bounds
static std::vector<float> s_buf;       // capture stream since record start (interleaved)
static size_t s_take_len   = 0;        // loop length in interleaved floats
static size_t s_lat_off    = 0;        // capture→loop-grid offset in interleaved floats
static int    s_take_count = 0;
static float  s_level      = 0.f;

// Live waveform bins for the pass in progress — per-bin RMS accumulated
// incrementally as fresh capture lands, then normalized to the running peak
// in recorder_live_peaks. Same math as waveform.cpp runs on the finished
// file, so the live wave looks like the placed take will (the old version
// drew raw unnormalized peaks of sparse samples — a mic peaking at 0.3
// rendered three times thinner live than placed).
static constexpr int LIVE_BINS = 2048;
static double   s_lb_sumsq[LIVE_BINS];
static uint32_t s_lb_cnt[LIVE_BINS];
static size_t   s_lb_scanned = 0;      // absolute s_buf offset already binned

static void live_bins_reset() {
    memset(s_lb_sumsq, 0, sizeof(s_lb_sumsq));
    memset(s_lb_cnt,   0, sizeof(s_lb_cnt));
}

// Fold fresh capture into the current pass's bins (mono = left channel).
static void live_bins_scan() {
    if (s_take_len == 0) return;
    size_t base = s_lat_off + (size_t)s_take_count * s_take_len;
    if (s_lb_scanned < base) s_lb_scanned = base;
    size_t i = s_lb_scanned & ~(size_t)1;  // keep L/R phase
    for (; i + 1 < s_buf.size(); i += 2) {
        size_t off = i - base;
        if (off >= s_take_len) break;      // next pass — binned after finalize
        int b = (int)((unsigned long long)off * LIVE_BINS / s_take_len);
        float s = s_buf[i];
        s_lb_sumsq[b] += (double)s * s;
        s_lb_cnt[b]   += 1;
    }
    s_lb_scanned = i;
}

// ── WAV io ────────────────────────────────────────────────────────────────────
// 16-bit PCM stereo 44100 — read by ffmpeg (audio_source_ensure) and every DAW.

static bool write_wav16(const std::string& path, const float* smp, size_t n) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    uint32_t data_bytes = (uint32_t)(n * 2);  // 16-bit
    uint32_t riff_sz    = 36 + data_bytes;
    uint16_t fmt = 1, ch = 2, bits = 16;
    uint32_t rate = 44100, byte_rate = rate * ch * 2;
    uint16_t block = ch * 2;
    uint32_t fmt_sz = 16;
    bool ok = true;
    auto put = [&](const void* p, size_t sz) { ok = ok && fwrite(p, 1, sz, f) == sz; };
    put("RIFF", 4); put(&riff_sz, 4); put("WAVE", 4);
    put("fmt ", 4); put(&fmt_sz, 4);
    put(&fmt, 2); put(&ch, 2); put(&rate, 4); put(&byte_rate, 4);
    put(&block, 2); put(&bits, 2);
    put("data", 4); put(&data_bytes, 4);
    std::vector<int16_t> pcm(n);
    for (size_t i = 0; i < n; ++i) {
        float v = fmaxf(-1.f, fminf(1.f, smp[i]));
        pcm[i] = (int16_t)lrintf(v * 32767.f);
    }
    put(pcm.data(), n * 2);
    fclose(f);
    if (!ok) ::unlink(path.c_str());
    return ok;
}

float recorder_wav_duration(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return 0.f;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    if (sz <= 44) return 0.f;
    return (float)(sz - 44) / (2.f /*bytes*/ * 2.f /*ch*/ * 44100.f);
}

// ── Helpers ───────────────────────────────────────────────────────────────────

static Clip* target_brick(AppState& state) {
    auto match = [&](int ti, int ci) -> Clip* {
        if (ti < 0 || ti >= (int)state.tracks.size()) return nullptr;
        auto& clips = state.tracks[ti].clips;
        if (ci < 0 || ci >= (int)clips.size()) return nullptr;
        Clip* cl = &clips[ci];
        if (cl->clip_type != ClipType::Record) return nullptr;
        // Identity check — track/clip indices shift when the timeline is
        // edited mid-record; the loop bounds are the brick's fingerprint.
        if (s_active && (fabsf(cl->start - s_lp_start) > 1e-4f ||
                         fabsf(cl->end   - s_lp_end)   > 1e-4f)) return nullptr;
        return cl;
    };
    if (Clip* cl = match(s_ti, s_ci)) return cl;
    if (!s_active) return nullptr;
    // Indices moved — rescan for the brick by its bounds.
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
    snprintf(buf, sizeof(buf), "%s/take_%ld_%02d.wav",
             takes_dir().c_str(), ms, s_take_count + 1);
    return buf;
}

static bool finalize_take(AppState& state, const float* smp, size_t n) {
    std::string path = next_take_path();
    if (!write_wav16(path, smp, n)) return false;
    Clip* cl = target_brick(state);
    if (!cl) { ::unlink(path.c_str()); return false; }
    cl->rec_takes.push_back(path);
    cl->rec_take_sel = (int)cl->rec_takes.size() - 1;
    ++s_take_count;
    return true;
}

// ── Public API ────────────────────────────────────────────────────────────────

bool recorder_live_peaks(int n, float* out) {
    if (!s_active || n <= 0 || s_take_len == 0) return false;
    memset(out, 0, (size_t)n * sizeof(float));

    // Per-bin RMS + running-peak normalization — the live preview of what
    // waveform.cpp will compute for the finished file. Unreached bins stay 0
    // (the timeline draws them flat).
    static float rms[LIVE_BINS];
    float norm = 0.02f;  // floor: don't blow room tone up to full height
    for (int b = 0; b < LIVE_BINS; ++b) {
        if (s_lb_cnt[b] == 0) { rms[b] = -1.f; continue; }
        rms[b] = sqrtf((float)(s_lb_sumsq[b] / (double)s_lb_cnt[b]));
        if (rms[b] > norm) norm = rms[b];
    }
    for (int ob = 0; ob < n; ++ob) {
        int b0 = (int)((long long)ob * LIVE_BINS / n);
        int b1 = (int)((long long)(ob + 1) * LIVE_BINS / n);
        if (b1 <= b0) b1 = b0 + 1;
        float v = -1.f;
        for (int b = b0; b < b1 && b < LIVE_BINS; ++b)
            if (rms[b] > v) v = rms[b];
        out[ob] = v < 0.f ? 0.f : fminf(1.f, v / norm);
    }
    return true;
}

bool recorder_active() { return s_active; }
bool recorder_is_target(int ti, int ci) { return s_active && ti == s_ti && ci == s_ci; }
int  recorder_take_count() { return s_take_count; }
float recorder_input_level() { return s_level; }

bool recorder_start(AppState& state, int ti, int ci) {
    if (s_active) return false;
    s_ti = ti; s_ci = ci;
    Clip* cl = target_brick(state);
    if (!cl || cl->end - cl->start < 0.25f) { s_ti = s_ci = -1; return false; }
    float lp_start = cl->start, lp_end = cl->end;
    s_lp_start = lp_start;
    s_lp_end   = lp_end;

    audio_init();
    if (!audio_capture_start()) { s_ti = s_ci = -1; return false; }

    // Wait for the capture device to actually deliver samples (spin-up can be
    // tens of ms); everything before playback starts is discarded so capture
    // frame 0 lines up with the loop start.
    std::vector<float> discard;
    for (int i = 0; i < 50; ++i) {
        audio_capture_drain(discard);
        if (!discard.empty()) break;
        usleep(5 * 1000);
    }

    audio_set_loop(lp_start, lp_end);
    audio_seek(lp_start);
    state.playhead        = lp_start;
    state.playing         = true;
    state.play_start_pos  = lp_start;
    state.play_start_wall = std::chrono::steady_clock::now();
    audio_play();
    discard.clear();
    audio_capture_drain(discard);  // drop pre-roll: stream origin = playback start

    live_bins_reset();
    s_lb_scanned = 0;
    s_take_len = (size_t)((lp_end - lp_start) * 44100.f) * 2;
    // The performer tracks what they hear (one output period late); the mic's
    // samples arrive one input period after that. Skipping that much of the
    // capture stream puts each take back on the loop grid.
    s_lat_off = (size_t)((audio_latency() + audio_capture_latency()) * 44100.f) * 2;
    s_buf.clear();
    s_take_count = 0;
    s_level      = 0.f;
    s_active     = true;
    return true;
}

void recorder_stop(AppState& state, bool keep_partial) {
    if (!s_active) return;
    audio_capture_drain(s_buf);
    // Keep the device running if the user is monitoring their mic; the
    // capture buffer self-wraps when nobody drains it.
    if (!audio_monitor_get()) audio_capture_stop();
    audio_clear_loop();

    // Keep a partial last pass when it has at least half a second in it —
    // stopping mid-phrase shouldn't throw the phrase away.
    size_t done = s_lat_off + (size_t)s_take_count * s_take_len;
    if (keep_partial && s_buf.size() > done + 44100u
        && s_take_len > 0 && s_buf.size() > s_lat_off) {
        size_t n = std::min(s_buf.size() - done, s_take_len);
        finalize_take(state, s_buf.data() + done, n);
    }

    if (s_take_count > 0)
        history_push(state, "Record " + std::to_string(s_take_count) + " take(s)");

    state.playing = false;
    audio_pause();

    s_active = false;
    s_ti = s_ci = -1;
    s_buf.clear();
    s_buf.shrink_to_fit();
    s_level = 0.f;
}

void recorder_tick(AppState& state) {
    if (!s_active) return;

    // Brick deleted / project swapped / user stopped the transport → wind down.
    if (!target_brick(state) || !state.playing || !audio_loop_active()) {
        bool keep = target_brick(state) != nullptr;
        recorder_stop(state, keep);
        return;
    }

    size_t before = s_buf.size();
    audio_capture_drain(s_buf);

    // Mic meter: peak of the fresh block, with decay so it falls smoothly.
    float peak = 0.f;
    for (size_t i = before; i < s_buf.size(); ++i)
        peak = fmaxf(peak, fabsf(s_buf[i]));
    s_level = fmaxf(peak, s_level * 0.90f);

    // Every full loop length past the latency offset is one finished take.
    while (s_take_len > 0 &&
           s_buf.size() >= s_lat_off + (size_t)(s_take_count + 1) * s_take_len) {
        if (!finalize_take(state,
                           s_buf.data() + s_lat_off + (size_t)s_take_count * s_take_len,
                           s_take_len)) {
            recorder_stop(state, false);  // disk/brick failure — don't spin
            return;
        }
        live_bins_reset();  // fresh pass starts on the brick
    }
    live_bins_scan();
}
