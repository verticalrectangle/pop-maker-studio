#include "audio.h"
#include "audio_fx.h"

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

// ── Input monitor ring ────────────────────────────────────────────────────────
// SPSC: capture callback writes, playback callback reads.
static constexpr uint32_t MON_N    = 1u << 15;   // 32768 floats ≈ 370 ms stereo
static constexpr uint32_t MON_MASK = MON_N - 1;
static float                 g_mon_ring[MON_N];
static std::atomic<uint32_t> g_mon_w{0}, g_mon_r{0};
static std::atomic<bool>     g_monitor_on{false};

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
    float tl_start, tl_end;
    float in_point, speed;
    float volume, pan;
    float fade_in, fade_out;
    PropTrack vol_keys, pan_keys;   // empty = use the static volume/pan
    std::shared_ptr<const std::vector<float>> buf;  // raw PCM; null = not loaded
    std::shared_ptr<FXBuf> fx_buf;  // non-null = processed samples available
};

static std::mutex            g_clip_mutex;
static std::vector<SrcBuf>   g_src_bufs;   // guarded by g_clip_mutex

// Published clip snapshot. The mixer callback must never wait on the UI
// thread (a missed block is an audible silence splice — "static"), so the UI
// builds a fresh immutable snapshot each frame and publishes it with a
// pointer swap; the callback's lock hold is one shared_ptr copy.
struct ClipSnapshot {
    std::vector<ClipInfo> clips;      // Audio-track clips
    std::vector<ClipInfo> vid_clips;  // Video-embedded audio
};
static std::mutex                          g_snap_mutex;
static std::shared_ptr<const ClipSnapshot> g_snap = std::make_shared<ClipSnapshot>();

// ── Audio callback ────────────────────────────────────────────────────────────

static void data_callback(ma_device* pDevice, void* pOutput, const void*, ma_uint32 frameCount) {
    if (g_loading.load()) {
        memset(pOutput, 0, frameCount * pDevice->playback.channels * sizeof(float));
        return;
    }
    float* out = (float*)pOutput;
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
        auto mix_clip = [&](const ClipInfo& cl, float global_vol) {
            if (t < cl.tl_start || t >= cl.tl_end) return;
            // Prefer FX-processed buffer when ready; fall back to raw samples.
            const std::vector<float>* buf_ptr = cl.buf ? cl.buf.get() : nullptr;
            if (cl.fx_buf && cl.fx_buf->ready.load(std::memory_order_acquire))
                buf_ptr = &cl.fx_buf->samples;
            if (!buf_ptr || buf_ptr->empty()) return;
            float src_t = cl.in_point + (t - cl.tl_start) * cl.speed;
            size_t sp = (size_t)(src_t * 44100.f) * 2;
            if (sp + 1 >= buf_ptr->size()) return;
            float fade = clip_fade(cl, t);
            float vraw = cl.vol_keys.empty() ? cl.volume
                       : cl.vol_keys.eval(t - cl.tl_start);
            float pan  = cl.pan_keys.empty() ? cl.pan
                       : fmaxf(-1.f, fminf(1.f, cl.pan_keys.eval(t - cl.tl_start)));
            float vol  = fmaxf(0.f, vraw) * global_vol * fade;
            float panL = pan <= 0.f ? 1.f : (1.f - pan);
            float panR = pan >= 0.f ? 1.f : (1.f + pan);
            out[f*2]   += (*buf_ptr)[sp]   * vol * panL;
            out[f*2+1] += (*buf_ptr)[sp+1] * vol * panR;
        };

        for (const auto& cl : snap->vid_clips) mix_clip(cl, g_volume);
        for (const auto& cl : snap->clips)     mix_clip(cl, 1.f);
    }

    // Input monitor: mix the live mic ring on top. If the ring backs up past
    // ~140 ms (clock drift, hiccup), skip ahead to ~46 ms so monitoring
    // latency stays bounded instead of slowly drifting into an echo.
    if (g_monitor_on.load(std::memory_order_relaxed)) {
        uint32_t w = g_mon_w.load(std::memory_order_acquire);
        uint32_t r = g_mon_r.load(std::memory_order_relaxed);
        if (w - r > 12288u) r = w - 4096u;
        for (size_t i = 0; i < need && r != w; ++i)
            out[i] += g_mon_ring[r++ & MON_MASK];
        g_mon_r.store(r, std::memory_order_release);
    }

    // Hard-clamp to prevent inter-clip summing from clipping.
    for (size_t i = 0; i < need; ++i)
        out[i] = fmaxf(-1.f, fminf(1.f, out[i]));
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
    if (g_device_init && !ma_device_is_started(&g_device)) ma_device_start(&g_device);
}

void audio_pause() {
    g_transport.store(false, std::memory_order_relaxed);
    // Keep the device alive for monitor-only output (mic in the mix while
    // the timeline is paused); otherwise stop it like before.
    bool monitor_solo = g_monitor_on.load(std::memory_order_relaxed) &&
                        audio_capture_active();
    if (g_device_init && !monitor_solo) ma_device_stop(&g_device);
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
    if (!g_device_init) return 0.f;
    ma_uint32 period = g_device.playback.internalPeriodSizeInFrames;
    ma_uint32 rate   = g_device.playback.internalSampleRate;
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

// ── Mic capture ───────────────────────────────────────────────────────────────

static ma_device         g_cap_device;
static bool              g_cap_init = false;
static std::mutex        g_cap_mutex;
static std::vector<float> g_cap_buf;  // interleaved stereo f32 @ 44100

// Device picker: enumeration context + current selection (-1 = default).
static ma_context g_ma_ctx;
static bool       g_ma_ctx_init = false;
static std::vector<std::string>  g_cap_dev_names;
static std::vector<ma_device_id> g_cap_dev_ids;
static int        g_cap_sel = -1;

static void capture_callback(ma_device*, void*, const void* pInput, ma_uint32 frameCount) {
    if (!pInput) return;
    const float* in = (const float*)pInput;
    const size_t n  = (size_t)frameCount * 2;

    if (g_monitor_on.load(std::memory_order_relaxed)) {
        uint32_t w = g_mon_w.load(std::memory_order_relaxed);
        uint32_t r = g_mon_r.load(std::memory_order_acquire);
        for (size_t i = 0; i < n && (w - r) < MON_N; ++i)
            g_mon_ring[w++ & MON_MASK] = in[i];
        g_mon_w.store(w, std::memory_order_release);
    }

    std::lock_guard<std::mutex> lk(g_cap_mutex);
    // Hard cap (~2 min): idle monitoring runs the device with nobody
    // draining, so wrap rather than grow unbounded. The recorder drains every
    // UI frame and discards pre-roll on start, so it never sees the wrap.
    if (g_cap_buf.size() > 44100u * 2u * 120u) g_cap_buf.clear();
    g_cap_buf.insert(g_cap_buf.end(), in, in + n);
}

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

void audio_monitor_set(bool on) {
    if (on) {
        // Start fresh — a stale ring would replay old input as a glitch.
        g_mon_r.store(g_mon_w.load(std::memory_order_relaxed),
                      std::memory_order_relaxed);
    }
    g_monitor_on.store(on, std::memory_order_relaxed);
    // Monitor-only output: run the device while the transport is paused so
    // the mic is audible; stop it again when monitoring ends while paused.
    if (!g_device_init) init_device();
    if (!g_device_init) return;
    bool transport = g_transport.load(std::memory_order_relaxed);
    if (on && !ma_device_is_started(&g_device)) ma_device_start(&g_device);
    else if (!on && !transport && ma_device_is_started(&g_device))
        ma_device_stop(&g_device);
}

bool audio_monitor_get() { return g_monitor_on.load(std::memory_order_relaxed); }

bool audio_capture_start() {
    if (g_cap_init) return true;
    ma_device_config cfg = ma_device_config_init(ma_device_type_capture);
    cfg.capture.format   = ma_format_f32;
    cfg.capture.channels = 2;   // miniaudio upmixes mono mics for us
    cfg.sampleRate       = 44100;
    cfg.dataCallback     = capture_callback;
    if (g_cap_sel >= 0 && g_cap_sel < (int)g_cap_dev_ids.size())
        cfg.capture.pDeviceID = &g_cap_dev_ids[g_cap_sel];
    ma_context* ctx = (cfg.capture.pDeviceID && ensure_ma_ctx()) ? &g_ma_ctx : nullptr;
    if (ma_device_init(ctx, &cfg, &g_cap_device) != MA_SUCCESS) return false;
    {
        std::lock_guard<std::mutex> lk(g_cap_mutex);
        g_cap_buf.clear();
    }
    g_mon_r.store(g_mon_w.load(std::memory_order_relaxed), std::memory_order_relaxed);
    if (ma_device_start(&g_cap_device) != MA_SUCCESS) {
        ma_device_uninit(&g_cap_device);
        return false;
    }
    g_cap_init = true;
    return true;
}

void audio_capture_stop() {
    if (!g_cap_init) return;
    ma_device_stop(&g_cap_device);
    ma_device_uninit(&g_cap_device);
    g_cap_init = false;
}

bool audio_capture_active() { return g_cap_init; }

void audio_capture_drain(std::vector<float>& out) {
    std::lock_guard<std::mutex> lk(g_cap_mutex);
    out.insert(out.end(), g_cap_buf.begin(), g_cap_buf.end());
    g_cap_buf.clear();
}

float audio_capture_latency() {
    if (!g_cap_init) return 0.f;
    ma_uint32 period = g_cap_device.capture.internalPeriodSizeInFrames;
    ma_uint32 rate   = g_cap_device.capture.internalSampleRate;
    if (rate == 0) return 0.f;
    return (float)period / (float)rate;
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
// g_src_bufs); FX cache uses its own lock.
static void clips_fill(std::vector<ClipInfo>& out, const std::vector<AudioClipDesc>& descs) {
    out.clear();
    for (const auto& d : descs) {
        ClipInfo ci;
        ci.tl_start = d.tl_start; ci.tl_end  = d.tl_end;
        ci.in_point = d.in_point; ci.speed    = d.speed;
        ci.volume   = d.volume;   ci.pan      = d.pan;
        ci.fade_in  = d.fade_in;  ci.fade_out = d.fade_out;
        ci.vol_keys = d.vol_keys; ci.pan_keys = d.pan_keys;
        for (auto& b : g_src_bufs) {
            if (b.path == d.path && b.ready) { ci.buf = b.samples; break; }
        }

        // FX processing — windowed segments, so a brick's effect lands only
        // on its own range of the source.
        if (d.fx_hash != 0 && !d.fx_segs.empty() && ci.buf) {
            auto key = std::make_pair(d.path, d.fx_hash);
            std::shared_ptr<FXBuf> fb;
            {
                std::lock_guard<std::mutex> lk2(g_fx_mutex);
                auto it = g_fx_cache.find(key);
                if (it != g_fx_cache.end()) {
                    fb = it->second;
                } else {
                    fb = std::make_shared<FXBuf>();
                    g_fx_cache[key] = fb;
                    // Kick off async processing. Capture the source as a
                    // shared_ptr — it used to copy the whole PCM buffer.
                    uint64_t my_gen = fb->gen.fetch_add(1) + 1;
                    std::shared_ptr<const std::vector<float>> src = ci.buf;
                    std::vector<AudioFXSegment> segs = d.fx_segs;
                    std::thread([fb, src, segs, my_gen]() {
                        auto result = process_audio_fx_segments(*src, segs, 44100.f,
                                                                &fb->gen, my_gen);
                        if (!result.empty()) {
                            fb->samples = std::move(result);
                            fb->ready.store(true, std::memory_order_release);
                        }
                    }).detach();
                }
            }
            ci.fx_buf = fb;
        }

        out.push_back(ci);
    }
}

// Publish one half of the snapshot, carrying the other half over.
static void snapshot_publish(std::vector<ClipInfo>&& filled, bool video_half) {
    auto ns = std::make_shared<ClipSnapshot>();
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

void audio_clips_update(const std::vector<AudioClipDesc>& descs) {
    std::vector<ClipInfo> filled;
    {
        std::lock_guard<std::mutex> lk(g_clip_mutex);
        clips_fill(filled, descs);
    }
    snapshot_publish(std::move(filled), false);
}

void video_audio_clips_update(const std::vector<AudioClipDesc>& descs) {
    std::vector<ClipInfo> filled;
    {
        std::lock_guard<std::mutex> lk(g_clip_mutex);
        clips_fill(filled, descs);
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
