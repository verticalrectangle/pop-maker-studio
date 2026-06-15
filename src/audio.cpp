#include "audio.h"
#include "audio_fx.h"
#ifdef HAVE_PIPEWIRE
#include "audio_pw.h"
#endif

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <map>
#include <memory>

#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <fcntl.h>

// ── Main audio buffer (background music / stems) ──────────────────────────────

static ma_device        g_device;
static ma_device_config g_device_cfg;
static bool             g_device_init = false;

static std::vector<float>   g_samples;           // interleaved stereo f32 @ 44100 Hz
static std::atomic<size_t>  g_read_pos{0};
static float                g_duration  = 0.f;
static float                g_volume    = 1.f;

static std::atomic<bool> g_loading{false};

// ── Loop region ───────────────────────────────────────────────────────────────
// Bounds are in g_read_pos units (interleaved floats: frame*2) so the callback
// can wrap with integer math only. end==0 means no loop.
static std::atomic<bool>     g_loop_on{false};
static std::atomic<size_t>   g_loop_start{0};
static std::atomic<size_t>   g_loop_end{0};
static std::atomic<uint64_t> g_loop_cycles{0};

// ── Performance mode (low-latency duplex monitor/record) ─────────────────────
// While the mic is open (monitoring or recording), the playback-only device
// is swapped for ONE duplex device with small periods: input → gain/gate →
// summed straight into the output buffer inside the same callback. No ring
// between capture and playback, no second device clock — round trip is one
// input period + one output period (~6 ms at 128 frames) instead of the old
// two-device SPSC path (~40 ms). Pattern lifted from Alexis's silvertune
// companion (single ma_device_type_duplex @ 128-frame periods).
static ma_device         g_duplex;
static bool              g_duplex_init = false;
static bool              g_cap_init    = false;  // perf mode active (either backend)
static bool              g_perf_pw     = false;  // native PipeWire backend in use
static std::atomic<bool> g_monitor_on{false};

// Monitor ring — SPSC: input block pushes the gated mic, output block drains
// it into the mix. On the miniaudio duplex backend both happen in the SAME
// callback (zero added latency); on the PipeWire backend the capture and
// playback streams tick the same graph clock, so the fill stays a constant
// one cycle (~3 ms) instead of drifting like the old two-device path.
static constexpr uint32_t MONR_N    = 1u << 15;   // 32768 floats ≈ 370 ms stereo
static constexpr uint32_t MONR_MASK = MONR_N - 1;
static float                 g_monr[MONR_N];
static std::atomic<uint32_t> g_monr_w{0}, g_monr_r{0};

// Capture ring — SPSC: duplex callback writes, audio_capture_drain (UI frame)
// reads. 2^21 floats ≈ 23.8 s stereo @ 44.1k; the writer drops samples when
// full rather than overwrite unread audio (the recorder drains every frame,
// so a full ring only happens in abandoned monitor-only runs).
static constexpr uint32_t CAP_N    = 1u << 21;
static constexpr uint32_t CAP_MASK = CAP_N - 1;
static float                 g_cap_ring[CAP_N];
static std::atomic<uint32_t> g_cap_w{0}, g_cap_r{0};
// Live input peak (0–1) of the most recent capture buffer — for a mic meter
// while monitoring, independent of whether anything is recording.
static std::atomic<float>    g_in_peak{0.f};

// Noise gate (silvertune companion port): leaky RMS² on the mono sum opens /
// closes a smoothed gain. Monitor audio is always gated when enabled; the
// recorded stream stays raw unless bake is on.
static std::atomic<bool> g_gate_on{true};
static std::atomic<bool> g_gate_bake{false};
static float             g_gate_energy = 1e-6f;   // audio thread only
static float             g_gate_gain   = 0.f;     // smoothed 0→1 gate state
static constexpr float GATE_THRESHOLD = 2e-5f;    // ~-47 dBFS RMS
static constexpr float GATE_ATTACK    = 0.9995f;  // ~21 ms attack
static constexpr float GATE_RELEASE   = 0.9999f;  // ~104 ms release

// "Hear effects": the record brick's audio FX chain applied to the MONITOR
// only (the take records dry; playback re-applies the same chain, so what
// you sing against is what the take becomes). UI thread builds/swaps the
// chain; the audio thread only loads the pointer once per block. Retired
// chains free after a grace period long enough for any in-flight block.
static std::atomic<bool>          g_mon_fx_on{false};
static std::atomic<AudioFXChain*> g_mon_chain{nullptr};

// Click-free device swaps: output ramps 0→1 over FADE_LEN frames after any
// device (re)start. Audio thread consumes; control thread arms via store(0).
static constexpr int       FADE_LEN = 1024;       // ~23 ms
static std::atomic<int>    g_fade_pos{FADE_LEN};

// Stall counter: the pulse shim delivers callbacks in bursts, so period-scale
// wall-clock gaps are normal and say nothing about dropped audio (verified:
// 143 "gaps" over a recording whose takes were sample-exact). Only a gap long
// enough to be an audible hole — graph stall, UI freeze, device hiccup — is
// worth counting. Reset on capture start, surfaced in the record panel.
static constexpr double      STALL_GAP_S = 0.050;
static std::atomic<uint32_t> g_xruns{0};
static double                g_last_cb_time = 0.0;  // audio thread only

// The output device can run for monitoring alone (hear the mic while the
// timeline is paused). The master clock advances and clips mix only while
// the transport is actually playing.
static std::atomic<bool>     g_transport{false};

// ── Per-clip source buffers ───────────────────────────────────────────────────

struct SrcBuf {
    std::string path;
    // Set exactly once when the decode lands and immutable afterwards, so
    // ClipInfo snapshots can hold the shared_ptr and read lock-free.
    std::shared_ptr<std::vector<float>> samples;  // interleaved stereo f32 @ 44100
    bool        ready = false;
};

// ── Processed FX cache ────────────────────────────────────────────────────────
// Keyed by (path, fx_hash). Shared_ptr so ClipInfo can hold a stable pointer.

struct FXBuf {
    std::vector<float>    samples;
    std::atomic<bool>     ready{false};
    std::atomic<uint64_t> gen{0};   // generation counter for cancellation
};

static std::mutex                                        g_fx_mutex;
static std::map<std::pair<std::string,uint64_t>, std::shared_ptr<FXBuf>> g_fx_cache;

// ─────────────────────────────────────────────────────────────────────────────

struct ClipInfo {
    int   track = 0;
    float tl_start, tl_end;
    float in_point, speed;
    float volume, pan;
    float fade_in, fade_out;
    PropTrack vol_keys, pan_keys;   // empty = use the static volume/pan
    std::shared_ptr<const std::vector<float>> buf;  // raw PCM; null = not loaded
    AudioFXChain* chain = nullptr;  // live FX (DAW-style); owned by g_chain_reg
};

// ── Live chain registry ───────────────────────────────────────────────────────
// One stateful chain per (half, path, tl_start): state survives the per-frame
// snapshot republish, dies with the clip. UI thread only (under g_clip_mutex);
// retired chains free after a grace period since the audio thread may still
// be inside a block that loaded the old snapshot.
struct ChainSlot {
    AudioFXChain* chain = nullptr;
    uint64_t      hash  = 0;
    double        last_use = 0.0;
};
static std::map<std::string, ChainSlot> g_chain_reg;
static std::vector<std::pair<AudioFXChain*, double>> g_chain_retire;

static double mono_now() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void chains_gc() {  // caller holds g_clip_mutex
    double now = mono_now();
    for (auto it = g_chain_reg.begin(); it != g_chain_reg.end(); ) {
        if (now - it->second.last_use > 1.0) {
            if (it->second.chain) g_chain_retire.push_back({it->second.chain, now});
            it = g_chain_reg.erase(it);
        } else ++it;
    }
    for (auto it = g_chain_retire.begin(); it != g_chain_retire.end(); ) {
        if (now - it->second > 0.25) {
            audio_fx_chain_free(it->first);
            it = g_chain_retire.erase(it);
        } else ++it;
    }
}

static std::mutex            g_clip_mutex;
static std::vector<SrcBuf>   g_src_bufs;   // guarded by g_clip_mutex

// Published clip snapshot. The mixer callback must never wait on the UI
// thread (a missed block is an audible silence splice — "static"), so the UI
// builds a fresh immutable snapshot each frame and publishes it with a
// pointer swap; the callback's lock hold is one shared_ptr copy.
static constexpr int AB_MAX = 8;   // max simultaneously-active bus bricks + direct
// One active bus brick in a published snapshot.
struct SnapBrick {
    int           track = 0;        // groups tracks > this
    float         start = 0.f, end = 0.f;
    AudioFXChain* chain = nullptr;  // owned by g_chain_reg
    float         gain  = 1.f;
    float         lat_s = 0.f;      // PDC read-ahead (seconds)
};
struct ClipSnapshot {
    std::vector<ClipInfo>  clips;      // Audio-track clips
    std::vector<ClipInfo>  vid_clips;  // Video-embedded audio
    std::vector<SnapBrick> bricks;     // bus bricks, ascending track
};
static std::mutex                          g_snap_mutex;
static std::shared_ptr<const ClipSnapshot> g_snap = std::make_shared<ClipSnapshot>();

// Current bus-brick config — written by audio_bus_bricks_update (UI thread,
// under g_clip_mutex), copied into every published snapshot.
static std::vector<SnapBrick> g_bus_bricks;

// ── Audio callback ────────────────────────────────────────────────────────────

// Master mix: advance the clock, render the clip snapshot into `out`.
// Shared by the playback-only callback and the duplex callback. No
// allocations; the only lock is the snapshot pointer swap (bounded).
static void mix_master(float* out, ma_uint32 frameCount) {
    if (g_loading.load()) {
        memset(out, 0, (size_t)frameCount * 2 * sizeof(float));
        return;
    }
    size_t pos  = g_read_pos.load(std::memory_order_relaxed);
    size_t need = (size_t)frameCount * 2;
    const bool transport = g_transport.load(std::memory_order_relaxed);

    // Advance the timeline clock first — g_read_pos is our master clock, not a source cursor.
    // With a loop region set, the clock wraps end → start; one increment of
    // g_loop_cycles per wrap is the recorder's take boundary signal.
    const bool   loop_on = g_loop_on.load(std::memory_order_relaxed);
    const size_t lp_s    = g_loop_start.load(std::memory_order_relaxed);
    const size_t lp_e    = g_loop_end.load(std::memory_order_relaxed);
    if (transport) {
        size_t new_pos = pos + need;
        if (loop_on && lp_e > lp_s) {
            while (new_pos >= lp_e) {
                new_pos = lp_s + (new_pos - lp_e);
                g_loop_cycles.fetch_add(1, std::memory_order_relaxed);
            }
        }
        g_read_pos.store(new_pos, std::memory_order_relaxed);
    }

    // Start with silence.
    memset(out, 0, need * sizeof(float));

    // Inline fade helper.
    auto clip_fade = [](const ClipInfo& cl, float t) -> float {
        float fade = 1.f;
        float dt_in  = t - cl.tl_start;
        float dt_out = cl.tl_end - t;
        if (cl.fade_in  > 0.f && dt_in  < cl.fade_in)  fade = dt_in / cl.fade_in;
        if (cl.fade_out > 0.f && dt_out < cl.fade_out) fade = fminf(fade, dt_out / cl.fade_out);
        return fade;
    };

    // Grab the published snapshot — bounded wait (publisher holds the lock
    // for one pointer swap), never a skipped block. Clips mix only while the
    // transport plays; a monitor-only device run outputs just the mic.
    std::shared_ptr<const ClipSnapshot> snap;
    if (transport) {
        std::lock_guard<std::mutex> lk(g_snap_mutex);
        snap = g_snap;
    }

    for (ma_uint32 f = 0; snap && f < frameCount; ++f) {
        // Per-frame position with loop wrap so the block that crosses the
        // boundary mixes the loop start, not audio past the loop end.
        size_t fpos = pos + (size_t)f * 2;
        if (loop_on && lp_e > lp_s)
            while (fpos >= lp_e) fpos = lp_s + (fpos - lp_e);
        float t = (float)(fpos / 2) / 44100.f;

        // ── All clips (Video + Audio) read from per-source PCM buffers ──────────
        // Video and Audio clips use the same source-buffer system so they layer
        // cleanly within their brick range regardless of clip type.
        // Owning bus brick for a clip at time t: the brick on the highest track
        // ABOVE the clip (brick.track < clip.track) whose span [start,end)
        // contains t. -1 = ungrouped (sums straight to output). Brick i → slot
        // i+1; slot 0 is the direct/ungrouped bus.
        auto owner_brick = [&](int cl_track) -> int {
            int best = -1, best_track = -1;
            for (int i = 0; i < (int)snap->bricks.size(); ++i) {
                const SnapBrick& b = snap->bricks[i];
                if (b.track < cl_track && t >= b.start && t < b.end && b.track > best_track) {
                    best_track = b.track; best = i;
                }
            }
            return best;
        };

        auto mix_clip = [&](const ClipInfo& cl, float global_vol,
                            float (*acc)[2]) {
            int ob = owner_brick(cl.track);
            // PDC: clips routed through a brick with grain FX read ahead by its
            // chain latency so the post-chain audio lands back on the grid.
            float tb = t + (ob >= 0 ? snap->bricks[ob].lat_s : 0.f);
            if (tb < cl.tl_start || tb >= cl.tl_end) return;
            const std::vector<float>* buf_ptr = cl.buf ? cl.buf.get() : nullptr;
            if (!buf_ptr || buf_ptr->empty()) return;
            float src_t = cl.in_point + (tb - cl.tl_start) * cl.speed;
            size_t sp = (size_t)(src_t * 44100.f) * 2;
            if (sp + 1 >= buf_ptr->size()) return;
            float sl = (*buf_ptr)[sp], sr2 = (*buf_ptr)[sp+1];
            // Live FX, pre-fader — same FXUnits the export bake runs.
            if (cl.chain)
                audio_fx_chain_process_seg(cl.chain, sl, sr2, src_t,
                                           (int64_t)(sp / 2));
            float fade = clip_fade(cl, tb);
            float vraw = cl.vol_keys.empty() ? cl.volume
                       : cl.vol_keys.eval(tb - cl.tl_start);
            float pan  = cl.pan_keys.empty() ? cl.pan
                       : fmaxf(-1.f, fminf(1.f, cl.pan_keys.eval(tb - cl.tl_start)));
            float vol  = fmaxf(0.f, vraw) * global_vol * fade;
            float panL = pan <= 0.f ? 1.f : (1.f - pan);
            float panR = pan >= 0.f ? 1.f : (1.f + pan);
            int slot = (ob >= 0 && ob + 1 < AB_MAX) ? ob + 1 : 0;
            acc[slot][0] += sl  * vol * panL;
            acc[slot][1] += sr2 * vol * panR;
        };

        float bus_acc[AB_MAX][2] = {};
        for (const auto& cl : snap->vid_clips) mix_clip(cl, g_volume, bus_acc);
        for (const auto& cl : snap->clips)     mix_clip(cl, 1.f, bus_acc);

        // Bus bricks → output: each brick runs its live chain on its submix,
        // then sums into the output with its gain. Ungrouped audio (slot 0)
        // passes straight through. Jump detection rides the master-clock frame
        // index, so seeks and loop wraps wipe tails.
        int64_t fidx = (int64_t)(fpos / 2);
        float mL = bus_acc[0][0], mR = bus_acc[0][1];
        for (int i = 0; i < (int)snap->bricks.size() && i + 1 < AB_MAX; ++i) {
            float L = bus_acc[i+1][0], R = bus_acc[i+1][1];
            if (snap->bricks[i].chain)
                audio_fx_chain_process_seg(snap->bricks[i].chain, L, R, t, fidx);
            mL += L * snap->bricks[i].gain;
            mR += R * snap->bricks[i].gain;
        }
        out[f*2]   += mL;
        out[f*2+1] += mR;
    }

    // Hard-clamp to prevent inter-clip summing from clipping.
    for (size_t i = 0; i < need; ++i)
        out[i] = fmaxf(-1.f, fminf(1.f, out[i]));
}

// Start-ramp: 0→1 over FADE_LEN frames after a device (re)start so the swap
// between the playback-only and duplex devices never clicks.
static void apply_fade_in(float* out, ma_uint32 frameCount) {
    int fp = g_fade_pos.load(std::memory_order_relaxed);
    if (fp >= FADE_LEN) return;
    for (ma_uint32 f = 0; f < frameCount; ++f) {
        float g = fp < FADE_LEN ? (float)fp / (float)FADE_LEN : 1.f;
        out[f*2]   *= g;
        out[f*2+1] *= g;
        ++fp;
    }
    g_fade_pos.store(fp, std::memory_order_relaxed);
}

// Normal mode: playback-only device, mix only.
static void data_callback(ma_device*, void* pOutput, const void*, ma_uint32 frameCount) {
    float* out = (float*)pOutput;
    mix_master(out, frameCount);
    apply_fade_in(out, frameCount);
}

// Performance-mode input: gate the live mic, push the gated signal to the
// monitor ring and the raw (or baked) signal to the capture ring. Runs on
// the audio thread of whichever backend is active.
static void perf_input_block(const float* in, uint32_t frameCount) {
    AudioFXChain* fxc = g_mon_fx_on.load(std::memory_order_relaxed)
                      ? g_mon_chain.load(std::memory_order_acquire) : nullptr;
    const bool gate = g_gate_on.load(std::memory_order_relaxed);
    const bool bake = g_gate_bake.load(std::memory_order_relaxed);
    const bool mon  = g_monitor_on.load(std::memory_order_relaxed);
    uint32_t cw = g_cap_w.load(std::memory_order_relaxed);
    const uint32_t cr = g_cap_r.load(std::memory_order_acquire);
    uint32_t mw = g_monr_w.load(std::memory_order_relaxed);
    const uint32_t mr = g_monr_r.load(std::memory_order_acquire);
    float in_pk = 0.f;
    for (uint32_t f = 0; f < frameCount; ++f) {
        float l = in[f*2], r2 = in[f*2+1];
        in_pk = fmaxf(in_pk, fmaxf(fabsf(l), fabsf(r2)));
        // Gate: leaky RMS² of the mono sum drives a smoothed 0→1 gain.
        float mono = 0.5f * (l + r2);
        g_gate_energy = 0.999f * g_gate_energy + 0.001f * mono * mono;
        float open  = g_gate_energy > GATE_THRESHOLD ? 1.f : 0.f;
        float coeff = open > g_gate_gain ? (1.f - GATE_ATTACK) : (1.f - GATE_RELEASE);
        g_gate_gain += coeff * (open - g_gate_gain);
        float gg = gate ? g_gate_gain : 1.f;
        if (mon && (mw - mr) < MONR_N - 2) {
            float ml = l * gg, mr2 = r2 * gg;
            if (fxc) audio_fx_chain_process(fxc, ml, mr2);
            g_monr[mw++ & MONR_MASK] = ml;
            g_monr[mw++ & MONR_MASK] = mr2;
        }
        // Recorded stream: raw by default; gated only when bake is on.
        float cl = bake ? l * gg : l, cr2 = bake ? r2 * gg : r2;
        if (cw - cr < CAP_N - 2) {
            g_cap_ring[cw++ & CAP_MASK] = cl;
            g_cap_ring[cw++ & CAP_MASK] = cr2;
        }
    }
    g_cap_w.store(cw, std::memory_order_release);
    g_in_peak.store(in_pk, std::memory_order_relaxed);
    g_monr_w.store(mw, std::memory_order_release);
}

// Performance-mode output: timeline mix + drain the monitor ring on top.
static void perf_output_block(float* out, uint32_t frameCount) {
    mix_master(out, frameCount);

    // Stall detection from callback wall-clock gaps (audio thread only).
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        double now = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
        if (g_last_cb_time > 0.0 && now - g_last_cb_time > STALL_GAP_S)
            g_xruns.fetch_add(1, std::memory_order_relaxed);
        g_last_cb_time = now;
    }

    if (g_monitor_on.load(std::memory_order_relaxed)) {
        uint32_t w = g_monr_w.load(std::memory_order_acquire);
        uint32_t r = g_monr_r.load(std::memory_order_relaxed);
        // Backlog past ~90 ms (start hiccup, stall) → skip to ~12 ms so the
        // monitor stays tight instead of turning into an echo.
        if (w - r > 8192u) r = w - 1024u;
        const uint32_t need = frameCount * 2;
        for (uint32_t i = 0; i < need && r != w; ++i)
            out[i] += g_monr[r++ & MONR_MASK];
        g_monr_r.store(r, std::memory_order_release);
        for (uint32_t i = 0; i < need; ++i)
            out[i] = fmaxf(-1.f, fminf(1.f, out[i]));
    }
    apply_fade_in(out, frameCount);
}

// miniaudio duplex backend (fallback when PipeWire isn't available): input
// is pushed and drained within the SAME callback, so the monitor ring adds
// zero latency here.
static void duplex_callback(ma_device*, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    if (pInput) perf_input_block((const float*)pInput, frameCount);
    perf_output_block((float*)pOutput, frameCount);
}

// ── Device init/shutdown ──────────────────────────────────────────────────────

static void init_device() {
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format   = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate        = 44100;
    cfg.dataCallback      = data_callback;
    if (ma_device_init(nullptr, &cfg, &g_device) == MA_SUCCESS) {
        g_device_cfg  = cfg;
        g_device_init = true;
    }
}

void audio_init() {
    if (!g_device_init) init_device();
}

void audio_shutdown() {
    g_transport.store(false, std::memory_order_relaxed);  // so capture_stop won't restart playback
    if (g_device_init) {
        ma_device_stop(&g_device);
        ma_device_uninit(&g_device);
        g_device_init = false;
    }
    audio_capture_stop();
    audio_clear_loop();
    g_samples.clear();
    g_read_pos.store(0, std::memory_order_relaxed);
    g_duration = 0.f;
}

// ── Main audio load ───────────────────────────────────────────────────────────

bool audio_loading() { return g_loading.load(); }

bool audio_load(const std::string& path) {
    audio_shutdown();
    g_loading.store(true);

    // Probe container duration synchronously (fast — header only).
    {
        AVFormatContext* fc = nullptr;
        if (avformat_open_input(&fc, path.c_str(), nullptr, nullptr) == 0) {
            if (fc->duration != AV_NOPTS_VALUE)
                g_duration = (float)fc->duration / (float)AV_TIME_BASE;
            avformat_close_input(&fc);
        }
    }

    std::thread([path]() {
        static const char* TMP = "/tmp/pms_audio_decode.raw";
        const char* args[] = {
            "ffmpeg", "-hide_banner", "-loglevel", "error",
            "-y", "-i", path.c_str(),
            "-vn", "-ar", "44100", "-ac", "2", "-f", "f32le", TMP,
            nullptr
        };
        pid_t pid = fork();
        if (pid == 0) {
            int dn = open("/dev/null", O_WRONLY);
            if (dn >= 0) { dup2(dn, STDOUT_FILENO); dup2(dn, STDERR_FILENO); close(dn); }
            execvp("ffmpeg", const_cast<char**>(args));
            _exit(127);
        }
        if (pid < 0) { g_loading.store(false); return; }
        int wstat = 0;
        waitpid(pid, &wstat, 0);
        if (!WIFEXITED(wstat) || WEXITSTATUS(wstat) != 0) {
            g_loading.store(false); return;
        }
        FILE* f = fopen(TMP, "rb");
        if (!f) { g_loading.store(false); return; }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f); rewind(f);
        if (sz > 0) {
            g_samples.resize((size_t)sz / sizeof(float));
            fread(g_samples.data(), sizeof(float), g_samples.size(), f);
        }
        fclose(f);
        g_duration = (float)g_samples.size() / 2.f / 44100.f;
        g_read_pos.store(0, std::memory_order_relaxed);

        if (!g_device_init) init_device();

        g_loading.store(false);
    }).detach();

    return true;
}

void audio_play() {
    g_transport.store(true, std::memory_order_relaxed);
    if (g_cap_init) {
        // Perf mode: the duplex device owns the output and is already running.
        if (g_duplex_init && !ma_device_is_started(&g_duplex)) {
            g_fade_pos.store(0, std::memory_order_relaxed);
            ma_device_start(&g_duplex);
        }
        return;
    }
    if (g_device_init && !ma_device_is_started(&g_device)) {
        g_fade_pos.store(0, std::memory_order_relaxed);
        ma_device_start(&g_device);
    }
}

void audio_pause() {
    g_transport.store(false, std::memory_order_relaxed);
    // Perf mode keeps the duplex device alive — the mic stays audible while
    // the timeline is paused (mix_master idles on !transport).
    if (g_cap_init) return;
    if (g_device_init) ma_device_stop(&g_device);
}
void audio_set_volume(float v) { g_volume = (v < 0.f) ? 0.f : (v > 4.f) ? 4.f : v; }

void audio_seek(float seconds) {
    if (g_loading.load()) return;
    size_t sample = (size_t)(seconds * 44100.f) * 2;
    if (!g_samples.empty())
        g_read_pos.store(sample < g_samples.size() ? sample : g_samples.size(),
                         std::memory_order_relaxed);
    else
        g_read_pos.store(sample, std::memory_order_relaxed);
}

float audio_duration()   { return g_duration; }
bool  audio_is_playing() { return g_device_init && ma_device_is_started(&g_device); }

float audio_position() {
    return (float)(g_read_pos.load(std::memory_order_relaxed) / 2) / 44100.f;
}

float audio_latency() {
#ifdef HAVE_PIPEWIRE
    if (g_cap_init && g_perf_pw) return audio_pw_period_s();
#endif
    // Whichever device currently owns the output.
    const ma_device* dev = g_cap_init && g_duplex_init ? &g_duplex
                         : g_device_init               ? &g_device : nullptr;
    if (!dev) return 0.f;
    ma_uint32 period = dev->playback.internalPeriodSizeInFrames;
    ma_uint32 rate   = dev->playback.internalSampleRate;
    if (rate == 0) return 0.f;
    return (float)period / (float)rate;
}

// ── Loop region ───────────────────────────────────────────────────────────────

void audio_set_loop(float start_sec, float end_sec) {
    if (end_sec <= start_sec) { audio_clear_loop(); return; }
    g_loop_start.store((size_t)(start_sec * 44100.f) * 2, std::memory_order_relaxed);
    g_loop_end.store((size_t)(end_sec * 44100.f) * 2, std::memory_order_relaxed);
    g_loop_on.store(true, std::memory_order_relaxed);
}

void audio_clear_loop() {
    g_loop_on.store(false, std::memory_order_relaxed);
}

bool     audio_loop_active() { return g_loop_on.load(std::memory_order_relaxed); }
uint64_t audio_loop_cycles() { return g_loop_cycles.load(std::memory_order_relaxed); }

// ── Mic capture (performance mode) ────────────────────────────────────────────
// audio_capture_start/stop = enter/leave performance mode: the playback-only
// device stops and the duplex device takes over the output (and the master
// clock with it — g_read_pos is shared, so the handoff is seamless).

// Device picker: enumeration context + current selection (-1 = default).
static ma_context g_ma_ctx;
static bool       g_ma_ctx_init = false;
static std::vector<std::string>  g_cap_dev_names;
static std::vector<ma_device_id> g_cap_dev_ids;
static int        g_cap_sel = -1;

static bool ensure_ma_ctx() {
    if (g_ma_ctx_init) return true;
    if (ma_context_init(nullptr, 0, nullptr, &g_ma_ctx) != MA_SUCCESS) return false;
    g_ma_ctx_init = true;
    return true;
}

std::vector<std::string> audio_capture_devices() {
    g_cap_dev_names.clear();
    g_cap_dev_ids.clear();
    if (!ensure_ma_ctx()) return g_cap_dev_names;
    ma_device_info* play = nullptr; ma_uint32 nplay = 0;
    ma_device_info* cap  = nullptr; ma_uint32 ncap  = 0;
    if (ma_context_get_devices(&g_ma_ctx, &play, &nplay, &cap, &ncap) != MA_SUCCESS)
        return g_cap_dev_names;
    for (ma_uint32 i = 0; i < ncap; ++i) {
        g_cap_dev_names.push_back(cap[i].name);
        g_cap_dev_ids.push_back(cap[i].id);
    }
    if (g_cap_sel >= (int)g_cap_dev_ids.size()) g_cap_sel = -1;
    return g_cap_dev_names;
}

void audio_capture_select(int index) {
    g_cap_sel = (index >= 0 && index < (int)g_cap_dev_ids.size()) ? index : -1;
}

int audio_capture_selected() { return g_cap_sel; }

std::string audio_capture_pulse_source(int index) {
    if (index < 0 || index >= (int)g_cap_dev_ids.size()) return std::string();
    return std::string(g_cap_dev_ids[(size_t)index].pulse);  // pulse name == pw node name
}

void audio_monitor_set(bool on) {
    // Monitoring needs the duplex device — enter performance mode if the
    // caller hasn't already (panel does, IPC paths may not).
    if (on && !g_cap_init && !audio_capture_start()) return;
    g_monitor_on.store(on, std::memory_order_relaxed);
}

bool audio_monitor_get() { return g_monitor_on.load(std::memory_order_relaxed); }

bool audio_capture_start() {
    if (g_cap_init) return true;

    // Fresh state for this perf-mode run (shared by both backends).
    g_cap_r.store(g_cap_w.load(std::memory_order_relaxed), std::memory_order_relaxed);
    g_monr_r.store(g_monr_w.load(std::memory_order_relaxed), std::memory_order_relaxed);
    g_gate_energy  = 1e-6f;
    g_gate_gain    = 0.f;
    g_xruns.store(0, std::memory_order_relaxed);
    g_last_cb_time = 0.0;
    g_fade_pos.store(0, std::memory_order_relaxed);

#ifdef HAVE_PIPEWIRE
    // Native PipeWire stream pair first — it gets the real 128-frame quantum
    // the pulse shim refuses (~6 ms round trip vs ~14 ms).
    {
        const char* target = nullptr;
        if (g_cap_sel >= 0 && g_cap_sel < (int)g_cap_dev_ids.size())
            target = g_cap_dev_ids[(size_t)g_cap_sel].pulse;  // pulse name == pw node name
        if (audio_pw_start(perf_input_block, perf_output_block, target)) {
            if (g_device_init && ma_device_is_started(&g_device))
                ma_device_stop(&g_device);
            g_perf_pw  = true;
            g_cap_init = true;
            return true;
        }
    }
#endif

    ma_device_config cfg = ma_device_config_init(ma_device_type_duplex);
    cfg.capture.format    = ma_format_f32;
    cfg.capture.channels  = 2;   // miniaudio upmixes mono mics for us
    cfg.playback.format   = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate        = 44100;
    cfg.dataCallback      = duplex_callback;
    cfg.periodSizeInFrames = 128;   // ~2.9 ms — the whole point of perf mode
    if (g_cap_sel >= 0 && g_cap_sel < (int)g_cap_dev_ids.size())
        cfg.capture.pDeviceID = &g_cap_dev_ids[g_cap_sel];
    ma_context* ctx = (cfg.capture.pDeviceID && ensure_ma_ctx()) ? &g_ma_ctx : nullptr;

    if (ma_device_init(ctx, &cfg, &g_duplex) != MA_SUCCESS) {
        // Some routes refuse small periods — retry at the backend default
        // before giving up (still beats the old two-device path).
        cfg.periodSizeInFrames = 0;
        if (ma_device_init(ctx, &cfg, &g_duplex) != MA_SUCCESS) return false;
    }
    g_duplex_init = true;

    if (ma_device_start(&g_duplex) != MA_SUCCESS) {
        ma_device_uninit(&g_duplex);
        g_duplex_init = false;
        return false;
    }
    // Output handoff: the duplex device owns the mix now. g_read_pos is
    // shared, so playback continues from the same master-clock position.
    if (g_device_init && ma_device_is_started(&g_device))
        ma_device_stop(&g_device);
    g_cap_init = true;
    return true;
}

void audio_capture_stop() {
    if (!g_cap_init) return;
    g_monitor_on.store(false, std::memory_order_relaxed);
#ifdef HAVE_PIPEWIRE
    if (g_perf_pw) {
        audio_pw_stop();
        g_perf_pw = false;
    } else
#endif
    {
        ma_device_stop(&g_duplex);
        ma_device_uninit(&g_duplex);
        g_duplex_init = false;
    }
    g_cap_init = false;
    // Hand the output back to the normal device if the transport is rolling.
    if (g_transport.load(std::memory_order_relaxed)) {
        if (!g_device_init) init_device();
        if (g_device_init && !ma_device_is_started(&g_device)) {
            g_fade_pos.store(0, std::memory_order_relaxed);
            ma_device_start(&g_device);
        }
    }
}

bool audio_capture_active() { return g_cap_init; }
float audio_input_peak() {
    return g_cap_init ? g_in_peak.load(std::memory_order_relaxed) : 0.f;
}

void audio_capture_drain(std::vector<float>& out) {
    const uint32_t w = g_cap_w.load(std::memory_order_acquire);
    uint32_t       r = g_cap_r.load(std::memory_order_relaxed);
    size_t n = (size_t)(w - r);
    if (n == 0) return;
    out.reserve(out.size() + n);
    while (r != w) out.push_back(g_cap_ring[r++ & CAP_MASK]);
    g_cap_r.store(r, std::memory_order_release);
}

float audio_capture_latency() {
    if (!g_cap_init) return 0.f;
#ifdef HAVE_PIPEWIRE
    if (g_perf_pw) return audio_pw_period_s();
#endif
    if (!g_duplex_init) return 0.f;
    ma_uint32 period = g_duplex.capture.internalPeriodSizeInFrames;
    ma_uint32 rate   = g_duplex.capture.internalSampleRate;
    if (rate == 0) return 0.f;
    return (float)period / (float)rate;
}

bool     audio_perf_mode()  { return g_cap_init; }
uint32_t audio_perf_xruns() { return g_xruns.load(std::memory_order_relaxed); }
void audio_gate_set(bool on)       { g_gate_on.store(on, std::memory_order_relaxed); }
bool audio_gate_get()              { return g_gate_on.load(std::memory_order_relaxed); }
void audio_gate_bake_set(bool on)  { g_gate_bake.store(on, std::memory_order_relaxed); }
bool audio_gate_bake_get()         { return g_gate_bake.load(std::memory_order_relaxed); }

void audio_monitor_fx_set(bool on) { g_mon_fx_on.store(on, std::memory_order_relaxed); }
bool audio_monitor_fx_get()        { return g_mon_fx_on.load(std::memory_order_relaxed); }

void audio_monitor_chain_set(const std::vector<AudioFX>& stages) {
    AudioFXChain* next = stages.empty()
                       ? nullptr : audio_fx_chain_create(stages, 44100.f);
    AudioFXChain* old = g_mon_chain.exchange(next, std::memory_order_acq_rel);
    if (old) {
        // Grace period: any in-flight audio block is ≤ a few ms; 20 ms is
        // generous. UI-thread sleep, imperceptible at chain-change cadence.
        struct timespec ts = {0, 20 * 1000 * 1000};
        nanosleep(&ts, nullptr);
        audio_fx_chain_free(old);
    }
}

bool audio_monitor_chain_active() {
    return g_mon_chain.load(std::memory_order_relaxed) != nullptr;
}

bool audio_probe(const std::string& path, AudioMeta& meta) {
    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, path.c_str(), nullptr, nullptr) < 0) return false;
    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        avformat_close_input(&fmt_ctx); return false;
    }
    meta.duration_secs = (fmt_ctx->duration != AV_NOPTS_VALUE && fmt_ctx->duration > 0)
                         ? (float)fmt_ctx->duration / (float)AV_TIME_BASE : 0.f;
    for (unsigned i = 0; i < fmt_ctx->nb_streams; ++i) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            meta.sample_rate = fmt_ctx->streams[i]->codecpar->sample_rate;
#if LIBAVUTIL_VERSION_MAJOR >= 57
            meta.channels    = fmt_ctx->streams[i]->codecpar->ch_layout.nb_channels;
#else
            meta.channels    = fmt_ctx->streams[i]->codecpar->channels;
#endif
            break;
        }
    }
    avformat_close_input(&fmt_ctx);
    return true;
}

// ── Clip-based audio ──────────────────────────────────────────────────────────

void audio_source_ensure(const std::string& path) {
    if (path.empty()) return;
    {
        std::lock_guard<std::mutex> lk(g_clip_mutex);
        for (auto& b : g_src_bufs)
            if (b.path == path) return;
        g_src_bufs.push_back({path, {}, false});
    }
    std::thread([path]() {
        // Use a path-derived temp file to avoid conflicts between concurrent loads.
        std::string tmp = "/tmp/pms_ca_" +
                          std::to_string(std::hash<std::string>{}(path)) + ".raw";
        const char* args[] = {
            "ffmpeg", "-hide_banner", "-loglevel", "error",
            "-y", "-i", path.c_str(),
            "-vn", "-ar", "44100", "-ac", "2", "-f", "f32le", tmp.c_str(),
            nullptr
        };
        pid_t pid = fork();
        if (pid == 0) {
            int dn = open("/dev/null", O_WRONLY);
            if (dn >= 0) { dup2(dn, STDOUT_FILENO); dup2(dn, STDERR_FILENO); close(dn); }
            execvp("ffmpeg", const_cast<char**>(args));
            _exit(127);
        }
        if (pid < 0) return;
        int st = 0; waitpid(pid, &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) return;

        FILE* f = fopen(tmp.c_str(), "rb");
        if (!f) return;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f); rewind(f);
        auto buf = std::make_shared<std::vector<float>>();
        if (sz > 0) {
            buf->resize((size_t)sz / sizeof(float));
            fread(buf->data(), sizeof(float), buf->size(), f);
        }
        fclose(f);

        std::lock_guard<std::mutex> lk(g_clip_mutex);
        for (auto& b : g_src_bufs) {
            if (b.path == path) {
                b.samples = std::move(buf);
                b.ready   = true;
                break;
            }
        }
    }).detach();
}

// Build ClipInfos for one snapshot half. Caller holds g_clip_mutex (for
// g_src_bufs and the chain registry). `half` namespaces the chain key so a
// video's embedded audio never shares DSP state with an audio clip that
// happens to have the same path+start.
static void clips_fill(std::vector<ClipInfo>& out, const std::vector<AudioClipDesc>& descs,
                       char half) {
    out.clear();
    for (const auto& d : descs) {
        ClipInfo ci;
        ci.track = d.track;
        ci.tl_start = d.tl_start; ci.tl_end  = d.tl_end;
        ci.in_point = d.in_point; ci.speed    = d.speed;
        ci.volume   = d.volume;   ci.pan      = d.pan;
        ci.fade_in  = d.fade_in;  ci.fade_out = d.fade_out;
        ci.vol_keys = d.vol_keys; ci.pan_keys = d.pan_keys;
        for (auto& b : g_src_bufs) {
            if (b.path == d.path && b.ready) { ci.buf = b.samples; break; }
        }

        // Live FX (DAW-style): one stateful chain per clip, rebuilt when the
        // effective segment set changes. Voice convert stays on its offline
        // path (vc_out_path source substitution).
        if (d.fx_hash != 0 && !d.fx_segs.empty() && ci.buf) {
            char keybuf[32];
            snprintf(keybuf, sizeof(keybuf), "%c%lld:", half,
                     (long long)llroundf(d.tl_start * 1000.f));
            std::string key = std::string(keybuf) + d.path;
            ChainSlot& slot = g_chain_reg[key];
            if (slot.hash != d.fx_hash || !slot.chain) {
                if (slot.chain) g_chain_retire.push_back({slot.chain, mono_now()});
                slot.chain = audio_fx_chain_create_seg(d.fx_segs, 44100.f);
                slot.hash  = d.fx_hash;
            }
            slot.last_use = mono_now();
            ci.chain = slot.chain;
        }

        out.push_back(ci);
    }
    chains_gc();
}

// Publish one half of the snapshot, carrying the other half over.
static void snapshot_publish(std::vector<ClipInfo>&& filled, bool video_half) {
    auto ns = std::make_shared<ClipSnapshot>();
    ns->bricks = g_bus_bricks;
    std::lock_guard<std::mutex> lk(g_snap_mutex);
    if (video_half) {
        ns->vid_clips = std::move(filled);
        ns->clips     = g_snap->clips;
    } else {
        ns->clips     = std::move(filled);
        ns->vid_clips = g_snap->vid_clips;
    }
    g_snap = std::move(ns);
}

void audio_bus_bricks_update(const std::vector<AudioBusBrick>& bricks) {
    std::lock_guard<std::mutex> lk(g_clip_mutex);
    std::vector<SnapBrick> out;
    out.reserve(bricks.size());
    for (int i = 0; i < (int)bricks.size() && i + 1 < AB_MAX; ++i) {
        const AudioBusBrick& d = bricks[(size_t)i];
        // Chain keyed by the brick's track+span so its DSP state survives the
        // per-frame republish (registry GC retires it when the brick is gone).
        char key[32];
        snprintf(key, sizeof(key), "BUS%d:%d", d.track,
                 (int)llroundf(d.start * 1000.f));
        ChainSlot& slot = g_chain_reg[key];
        uint64_t want = !d.stages.empty() ? (d.hash | 1ull) : 0ull;
        if (slot.hash != want) {
            if (slot.chain) g_chain_retire.push_back({slot.chain, mono_now()});
            slot.chain = want ? audio_fx_chain_create(d.stages, 44100.f) : nullptr;
            slot.hash  = want;
        }
        slot.last_use = mono_now();
        SnapBrick sb;
        sb.track = d.track; sb.start = d.start; sb.end = d.end;
        sb.gain  = d.gain;  sb.chain = slot.chain;
        sb.lat_s = slot.chain
            ? (float)audio_fx_chain_latency_frames(slot.chain) / 44100.f : 0.f;
        out.push_back(sb);
    }
    g_bus_bricks = std::move(out);
}

void audio_clips_update(const std::vector<AudioClipDesc>& descs) {
    std::vector<ClipInfo> filled;
    {
        std::lock_guard<std::mutex> lk(g_clip_mutex);
        clips_fill(filled, descs, 'A');
    }
    snapshot_publish(std::move(filled), false);
}

void video_audio_clips_update(const std::vector<AudioClipDesc>& descs) {
    std::vector<ClipInfo> filled;
    {
        std::lock_guard<std::mutex> lk(g_clip_mutex);
        clips_fill(filled, descs, 'V');
    }
    snapshot_publish(std::move(filled), true);
}

bool audio_source_cached(const std::string& path, std::vector<float>& out) {
    std::lock_guard<std::mutex> lk(g_clip_mutex);
    for (auto& b : g_src_bufs) {
        if (b.path == path && b.ready && b.samples && !b.samples->empty()) {
            out = *b.samples;
            return true;
        }
    }
    return false;
}

bool audio_fx_cached(const std::string& path, uint64_t fx_hash, std::vector<float>& out) {
    std::lock_guard<std::mutex> lk(g_fx_mutex);
    auto it = g_fx_cache.find(std::make_pair(path, fx_hash));
    if (it == g_fx_cache.end()) return false;
    if (!it->second->ready.load(std::memory_order_acquire)) return false;
    out = it->second->samples;
    return true;
}

void audio_clips_clear() {
    {
        std::lock_guard<std::mutex> lk(g_clip_mutex);
        g_src_bufs.clear();
        // Snapshot is replaced below, so the audio thread can't be handed a
        // dying chain after this point; the grace list covers in-flight blocks.
        for (auto& [k, slot] : g_chain_reg)
            if (slot.chain) g_chain_retire.push_back({slot.chain, mono_now()});
        g_chain_reg.clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_snap_mutex);
        g_snap = std::make_shared<ClipSnapshot>();
    }
    std::lock_guard<std::mutex> lk2(g_fx_mutex);
    // Bump generation on all in-flight jobs so they self-cancel, then evict.
    for (auto& [k, fb] : g_fx_cache) fb->gen.fetch_add(1);
    g_fx_cache.clear();
}
