#ifdef HAVE_PIPEWIRE
#include "audio_pw.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include <atomic>
#include <cstring>

static pw_thread_loop* g_pw_loop = nullptr;
static pw_stream*      g_pw_out  = nullptr;
static pw_stream*      g_pw_in   = nullptr;
static PwInProcess     g_in_cb   = nullptr;
static PwOutProcess    g_out_cb  = nullptr;
static bool            g_pw_on   = false;

static std::atomic<uint32_t> g_cycle_frames{128};

static void on_out_process(void*) {
    pw_buffer* b = pw_stream_dequeue_buffer(g_pw_out);
    if (!b) return;
    spa_data& d = b->buffer->datas[0];
    float* dst = (float*)d.data;
    if (!dst) { pw_stream_queue_buffer(g_pw_out, b); return; }
    const uint32_t stride = 2 * sizeof(float);
    uint32_t max_frames = d.maxsize / stride;
    uint32_t frames = b->requested ? (uint32_t)b->requested : max_frames;
    if (frames > max_frames) frames = max_frames;
    g_cycle_frames.store(frames, std::memory_order_relaxed);
    g_out_cb(dst, frames);
    d.chunk->offset = 0;
    d.chunk->stride = (int32_t)stride;
    d.chunk->size   = frames * stride;
    pw_stream_queue_buffer(g_pw_out, b);
}

static void on_in_process(void*) {
    pw_buffer* b = pw_stream_dequeue_buffer(g_pw_in);
    if (!b) return;
    spa_data& d = b->buffer->datas[0];
    if (d.data && d.chunk && d.chunk->size > 0) {
        const uint32_t stride = d.chunk->stride > 0 ? (uint32_t)d.chunk->stride
                                                    : 2 * sizeof(float);
        const float* src = (const float*)((const uint8_t*)d.data + d.chunk->offset);
        g_in_cb(src, d.chunk->size / stride);
    }
    pw_stream_queue_buffer(g_pw_in, b);
}

// pw_stream_events has a long member list; C++ can't partially designate, so
// fill the two we need at runtime.
static pw_stream_events g_out_events;
static pw_stream_events g_in_events;

static const spa_pod* stereo_44k_format(spa_pod_builder* pb) {
    spa_audio_info_raw info;
    memset(&info, 0, sizeof(info));
    info.format      = SPA_AUDIO_FORMAT_F32;   // interleaved
    info.rate        = 44100;
    info.channels    = 2;
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
    return spa_format_audio_raw_build(pb, SPA_PARAM_EnumFormat, &info);
}

bool audio_pw_start(PwInProcess in_cb, PwOutProcess out_cb, const char* capture_node) {
    if (g_pw_on) return true;
    g_in_cb  = in_cb;
    g_out_cb = out_cb;

    pw_init(nullptr, nullptr);
    g_pw_loop = pw_thread_loop_new("pms-perf-audio", nullptr);
    if (!g_pw_loop) return false;
    if (pw_thread_loop_start(g_pw_loop) != 0) {
        pw_thread_loop_destroy(g_pw_loop);
        g_pw_loop = nullptr;
        return false;
    }

    memset(&g_out_events, 0, sizeof(g_out_events));
    g_out_events.version = PW_VERSION_STREAM_EVENTS;
    g_out_events.process = on_out_process;
    memset(&g_in_events, 0, sizeof(g_in_events));
    g_in_events.version = PW_VERSION_STREAM_EVENTS;
    g_in_events.process = on_in_process;

    pw_thread_loop_lock(g_pw_loop);

    const auto flags = (pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT |
                                         PW_STREAM_FLAG_MAP_BUFFERS |
                                         PW_STREAM_FLAG_RT_PROCESS);
    uint8_t podbuf[1024];

    g_pw_out = pw_stream_new_simple(
        pw_thread_loop_get_loop(g_pw_loop), "Pop Maker Studio",
        pw_properties_new(PW_KEY_MEDIA_TYPE,     "Audio",
                          PW_KEY_MEDIA_CATEGORY, "Playback",
                          PW_KEY_MEDIA_ROLE,     "Production",
                          PW_KEY_NODE_LATENCY,   "128/44100",
                          nullptr),
        &g_out_events, nullptr);
    if (g_pw_out) {
        spa_pod_builder pb = SPA_POD_BUILDER_INIT(podbuf, sizeof(podbuf));
        const spa_pod* params[1] = { stereo_44k_format(&pb) };
        if (pw_stream_connect(g_pw_out, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                              flags, params, 1) < 0) {
            pw_stream_destroy(g_pw_out);
            g_pw_out = nullptr;
        }
    }

    pw_properties* in_props =
        pw_properties_new(PW_KEY_MEDIA_TYPE,     "Audio",
                          PW_KEY_MEDIA_CATEGORY, "Capture",
                          PW_KEY_MEDIA_ROLE,     "Production",
                          PW_KEY_NODE_LATENCY,   "128/44100",
                          nullptr);
    if (capture_node && capture_node[0])
        pw_properties_set(in_props, PW_KEY_TARGET_OBJECT, capture_node);
    g_pw_in = pw_stream_new_simple(
        pw_thread_loop_get_loop(g_pw_loop), "Pop Maker Studio Mic",
        in_props, &g_in_events, nullptr);
    if (g_pw_in) {
        spa_pod_builder pb = SPA_POD_BUILDER_INIT(podbuf, sizeof(podbuf));
        const spa_pod* params[1] = { stereo_44k_format(&pb) };
        if (pw_stream_connect(g_pw_in, PW_DIRECTION_INPUT, PW_ID_ANY,
                              flags, params, 1) < 0) {
            pw_stream_destroy(g_pw_in);
            g_pw_in = nullptr;
        }
    }

    pw_thread_loop_unlock(g_pw_loop);

    if (!g_pw_out || !g_pw_in) {
        audio_pw_stop();
        return false;
    }

    // Wait (bounded) for both streams to reach STREAMING — an error or a
    // timeout means PipeWire isn't going to schedule us; fall back.
    for (int i = 0; i < 100; ++i) {  // ≤ 2 s
        pw_thread_loop_lock(g_pw_loop);
        pw_stream_state so = pw_stream_get_state(g_pw_out, nullptr);
        pw_stream_state si = pw_stream_get_state(g_pw_in, nullptr);
        pw_thread_loop_unlock(g_pw_loop);
        if (so == PW_STREAM_STATE_ERROR || si == PW_STREAM_STATE_ERROR) {
            audio_pw_stop();
            return false;
        }
        if (so == PW_STREAM_STATE_STREAMING && si == PW_STREAM_STATE_STREAMING) {
            g_pw_on = true;
            return true;
        }
        struct timespec ts = {0, 20 * 1000 * 1000};
        nanosleep(&ts, nullptr);
    }
    audio_pw_stop();
    return false;
}

void audio_pw_stop() {
    if (!g_pw_loop) return;
    pw_thread_loop_lock(g_pw_loop);
    if (g_pw_in)  { pw_stream_destroy(g_pw_in);  g_pw_in  = nullptr; }
    if (g_pw_out) { pw_stream_destroy(g_pw_out); g_pw_out = nullptr; }
    pw_thread_loop_unlock(g_pw_loop);
    pw_thread_loop_stop(g_pw_loop);
    pw_thread_loop_destroy(g_pw_loop);
    g_pw_loop = nullptr;
    g_pw_on   = false;
}

bool audio_pw_active() { return g_pw_on; }

// ── Virtual microphone (media.class Audio/Source) ─────────────────────────────
static pw_thread_loop* g_vm_loop = nullptr;
static pw_stream*      g_vm_stream = nullptr;
static pw_stream_events g_vm_events;
static std::atomic<bool> g_vm_on{false};

// SPSC ring: audio thread writes, PipeWire RT thread reads.
static constexpr uint32_t VM_N    = 1 << 15;          // 32768 floats ≈ 370 ms stereo
static constexpr uint32_t VM_MASK = VM_N - 1;
static float                 g_vm_ring[VM_N];
static std::atomic<uint32_t> g_vm_w{0}, g_vm_r{0};

static void on_vm_process(void*) {
    pw_buffer* b = pw_stream_dequeue_buffer(g_vm_stream);
    if (!b) return;
    spa_data& d = b->buffer->datas[0];
    float* dst = (float*)d.data;
    if (!dst) { pw_stream_queue_buffer(g_vm_stream, b); return; }
    const uint32_t stride = 2 * sizeof(float);
    uint32_t max_frames = d.maxsize / stride;
    uint32_t frames = b->requested ? (uint32_t)b->requested : max_frames;
    if (frames > max_frames) frames = max_frames;
    uint32_t r = g_vm_r.load(std::memory_order_relaxed);
    const uint32_t w = g_vm_w.load(std::memory_order_acquire);
    uint32_t avail = (w - r) / 2;                     // frames buffered
    for (uint32_t f = 0; f < frames; ++f) {
        if (f < avail) {
            dst[f*2]   = g_vm_ring[r++ & VM_MASK];
            dst[f*2+1] = g_vm_ring[r++ & VM_MASK];
        } else {
            dst[f*2] = dst[f*2+1] = 0.f;              // underrun → silence
        }
    }
    g_vm_r.store(r, std::memory_order_release);
    d.chunk->offset = 0;
    d.chunk->stride = (int32_t)stride;
    d.chunk->size   = frames * stride;
    pw_stream_queue_buffer(g_vm_stream, b);
}

void audio_pw_vmic_push(const float* lr, uint32_t frames) {
    if (!g_vm_on.load(std::memory_order_relaxed)) return;
    uint32_t w = g_vm_w.load(std::memory_order_relaxed);
    const uint32_t r = g_vm_r.load(std::memory_order_acquire);
    for (uint32_t f = 0; f < frames; ++f) {
        if ((w - r) >= VM_N - 2) break;               // full → drop newest
        g_vm_ring[w++ & VM_MASK] = lr[f*2];
        g_vm_ring[w++ & VM_MASK] = lr[f*2+1];
    }
    g_vm_w.store(w, std::memory_order_release);
}

bool audio_pw_vmic_start() {
    if (g_vm_on.load()) return true;
    pw_init(nullptr, nullptr);                        // idempotent
    g_vm_loop = pw_thread_loop_new("pms-virtual-mic", nullptr);
    if (!g_vm_loop) return false;
    if (pw_thread_loop_start(g_vm_loop) != 0) {
        pw_thread_loop_destroy(g_vm_loop);
        g_vm_loop = nullptr;
        return false;
    }
    memset(&g_vm_events, 0, sizeof(g_vm_events));
    g_vm_events.version = PW_VERSION_STREAM_EVENTS;
    g_vm_events.process = on_vm_process;

    pw_thread_loop_lock(g_vm_loop);
    uint8_t podbuf[1024];
    g_vm_stream = pw_stream_new_simple(
        pw_thread_loop_get_loop(g_vm_loop), "Pop Maker Studio Mic",
        pw_properties_new(PW_KEY_MEDIA_TYPE,       "Audio",
                          PW_KEY_MEDIA_CATEGORY,   "Playback",
                          PW_KEY_MEDIA_CLASS,      "Audio/Source",
                          PW_KEY_MEDIA_ROLE,       "Communication",
                          PW_KEY_NODE_NAME,        "pms-virtual-mic",
                          PW_KEY_NODE_DESCRIPTION, "Pop Maker Studio Mic",
                          PW_KEY_NODE_LATENCY,     "128/44100",
                          nullptr),
        &g_vm_events, nullptr);
    if (g_vm_stream) {
        spa_pod_builder pb = SPA_POD_BUILDER_INIT(podbuf, sizeof(podbuf));
        const spa_pod* params[1] = { stereo_44k_format(&pb) };
        if (pw_stream_connect(g_vm_stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                              (pw_stream_flags)(PW_STREAM_FLAG_MAP_BUFFERS |
                                                PW_STREAM_FLAG_RT_PROCESS),
                              params, 1) < 0) {
            pw_stream_destroy(g_vm_stream);
            g_vm_stream = nullptr;
        }
    }
    pw_thread_loop_unlock(g_vm_loop);
    if (!g_vm_stream) { audio_pw_vmic_stop(); return false; }
    g_vm_w.store(0); g_vm_r.store(0);
    g_vm_on.store(true);
    return true;
}

void audio_pw_vmic_stop() {
    g_vm_on.store(false);
    if (!g_vm_loop) return;
    pw_thread_loop_lock(g_vm_loop);
    if (g_vm_stream) { pw_stream_destroy(g_vm_stream); g_vm_stream = nullptr; }
    pw_thread_loop_unlock(g_vm_loop);
    pw_thread_loop_stop(g_vm_loop);
    pw_thread_loop_destroy(g_vm_loop);
    g_vm_loop = nullptr;
}

bool audio_pw_vmic_active() { return g_vm_on.load(); }

float audio_pw_period_s() {
    return (float)g_cycle_frames.load(std::memory_order_relaxed) / 44100.f;
}
#endif  // HAVE_PIPEWIRE
