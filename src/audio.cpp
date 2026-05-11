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

// ── Per-clip source buffers ───────────────────────────────────────────────────

struct SrcBuf {
    std::string        path;
    std::vector<float> samples;  // interleaved stereo f32 @ 44100
    bool               ready = false;
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
    int   buf_idx;  // index into g_src_bufs, -1 = not yet loaded
    std::shared_ptr<FXBuf> fx_buf;  // non-null = processed samples available
};

static std::mutex            g_clip_mutex;
static std::vector<SrcBuf>   g_src_bufs;   // guarded by g_clip_mutex
static std::vector<ClipInfo> g_clips;      // Audio-track clips
static std::vector<ClipInfo> g_vid_clips;  // Video-embedded clips (read from g_samples)

// ── Audio callback ────────────────────────────────────────────────────────────

static void data_callback(ma_device* pDevice, void* pOutput, const void*, ma_uint32 frameCount) {
    if (g_loading.load()) {
        memset(pOutput, 0, frameCount * pDevice->playback.channels * sizeof(float));
        return;
    }
    float* out = (float*)pOutput;
    size_t pos  = g_read_pos.load(std::memory_order_relaxed);
    size_t need = (size_t)frameCount * 2;

    // Advance the timeline clock first — g_read_pos is our master clock, not a source cursor.
    g_read_pos.store(pos + need, std::memory_order_relaxed);

    // Start with silence.
    memset(out, 0, need * sizeof(float));

    // Inline fade helper.
    auto clip_fade = [](const ClipInfo& cl, float t) -> float {
        float fade = 1.f;
        float dt_in  = t - cl.tl_start;
        float dt_out = cl.tl_end - t;
        if (cl.fade_in  > 0.f && dt_in  < cl.fade_in)  fade = dt_in / cl.fade_in;
        if (cl.fade_out > 0.f && dt_out < cl.fade_out) fade = std::fminf(fade, dt_out / cl.fade_out);
        return fade;
    };

    std::unique_lock<std::mutex> lk(g_clip_mutex, std::try_to_lock);
    if (!lk.owns_lock()) return;

    float t0 = (float)(pos / 2) / 44100.f;

    for (ma_uint32 f = 0; f < frameCount; ++f) {
        float t = t0 + (float)f / 44100.f;

        // ── All clips (Video + Audio) read from per-source PCM buffers ──────────
        // Video and Audio clips use the same g_src_bufs system so they layer
        // cleanly within their brick range regardless of clip type.
        auto mix_clip = [&](const ClipInfo& cl, float global_vol) {
            if (t < cl.tl_start || t >= cl.tl_end) return;
            if (cl.buf_idx < 0 || cl.buf_idx >= (int)g_src_bufs.size()) return;
            // Prefer FX-processed buffer when ready; fall back to raw samples.
            const std::vector<float>* buf_ptr = &g_src_bufs[cl.buf_idx].samples;
            if (cl.fx_buf && cl.fx_buf->ready.load(std::memory_order_acquire))
                buf_ptr = &cl.fx_buf->samples;
            if (buf_ptr->empty()) return;
            float src_t = cl.in_point + (t - cl.tl_start) * cl.speed;
            size_t sp = (size_t)(src_t * 44100.f) * 2;
            if (sp + 1 >= buf_ptr->size()) return;
            float fade = clip_fade(cl, t);
            float vol  = cl.volume * global_vol * fade;
            float panL = cl.pan <= 0.f ? 1.f : (1.f - cl.pan);
            float panR = cl.pan >= 0.f ? 1.f : (1.f + cl.pan);
            out[f*2]   += (*buf_ptr)[sp]   * vol * panL;
            out[f*2+1] += (*buf_ptr)[sp+1] * vol * panR;
        };

        for (const auto& cl : g_vid_clips) mix_clip(cl, g_volume);
        for (const auto& cl : g_clips)     mix_clip(cl, 1.f);
    }

    // Hard-clamp to prevent inter-clip summing from clipping.
    for (size_t i = 0; i < need; ++i)
        out[i] = std::fmaxf(-1.f, std::fminf(1.f, out[i]));
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

void audio_play()              { if (g_device_init) ma_device_start(&g_device); }
void audio_pause()             { if (g_device_init) ma_device_stop(&g_device); }
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
            meta.channels    = fmt_ctx->streams[i]->codecpar->ch_layout.nb_channels;
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
        std::vector<float> buf;
        if (sz > 0) {
            buf.resize((size_t)sz / sizeof(float));
            fread(buf.data(), sizeof(float), buf.size(), f);
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

static void clips_fill(std::vector<ClipInfo>& out, const std::vector<AudioClipDesc>& descs) {
    out.clear();
    for (const auto& d : descs) {
        ClipInfo ci;
        ci.tl_start = d.tl_start; ci.tl_end  = d.tl_end;
        ci.in_point = d.in_point; ci.speed    = d.speed;
        ci.volume   = d.volume;   ci.pan      = d.pan;
        ci.fade_in  = d.fade_in;  ci.fade_out = d.fade_out;
        ci.buf_idx  = -1;
        for (int i = 0; i < (int)g_src_bufs.size(); ++i) {
            if (g_src_bufs[i].path == d.path && g_src_bufs[i].ready) {
                ci.buf_idx = i; break;
            }
        }

        // FX processing
        if (d.fx_hash != 0 && ci.buf_idx >= 0) {
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
                    // Kick off async processing
                    uint64_t my_gen = fb->gen.fetch_add(1) + 1;
                    const std::vector<float>& raw = g_src_bufs[ci.buf_idx].samples;
                    AudioFX fx = d.fx;
                    std::thread([fb, raw, fx, my_gen]() {
                        auto result = process_audio_fx(raw, fx, 44100.f,
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

void audio_clips_update(const std::vector<AudioClipDesc>& descs) {
    std::lock_guard<std::mutex> lk(g_clip_mutex);
    clips_fill(g_clips, descs);
}

void video_audio_clips_update(const std::vector<AudioClipDesc>& descs) {
    std::lock_guard<std::mutex> lk(g_clip_mutex);
    clips_fill(g_vid_clips, descs);
}

void audio_clips_clear() {
    {
        std::lock_guard<std::mutex> lk(g_clip_mutex);
        g_src_bufs.clear();
        g_clips.clear();
        g_vid_clips.clear();
    }
    std::lock_guard<std::mutex> lk2(g_fx_mutex);
    // Bump generation on all in-flight jobs so they self-cancel, then evict.
    for (auto& [k, fb] : g_fx_cache) fb->gen.fetch_add(1);
    g_fx_cache.clear();
}
