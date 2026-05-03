#include "audio.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <vector>
#include <thread>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <cstdlib>

#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <fcntl.h>

// ── PCM playback via miniaudio ────────────────────────────────────────────────

static ma_device        g_device;
static ma_device_config g_device_cfg;
static bool             g_device_init = false;

static std::vector<float> g_samples;   // interleaved stereo f32 @ 44100 Hz
static size_t             g_read_pos  = 0;
static float              g_duration  = 0.f;
static float              g_volume    = 1.f;

// True while the ffmpeg subprocess is running and samples are not ready yet.
static std::atomic<bool> g_loading{false};

static void data_callback(ma_device* pDevice, void* pOutput, const void*, ma_uint32 frameCount) {
    if (g_loading.load()) {
        memset(pOutput, 0, frameCount * pDevice->playback.channels * sizeof(float));
        return;
    }
    auto* out = (float*)pOutput;
    size_t available = (g_samples.size() > g_read_pos) ? (g_samples.size() - g_read_pos) : 0;
    size_t need      = frameCount * (size_t)pDevice->playback.channels;
    size_t copy      = (available < need) ? available : need;
    float  vol       = g_volume;
    for (size_t i = 0; i < copy; ++i)
        out[i] = g_samples[g_read_pos + i] * vol;
    if (copy < need)
        memset(out + copy, 0, (need - copy) * sizeof(float));
    g_read_pos += copy;
}

void audio_init() {}

void audio_shutdown() {
    if (g_device_init) {
        ma_device_stop(&g_device);
        ma_device_uninit(&g_device);
        g_device_init = false;
    }
}

bool audio_loading() { return g_loading.load(); }

bool audio_load(const std::string& path) {
    audio_shutdown();
    g_samples.clear();
    g_read_pos = 0;
    g_duration = 0.f;
    g_loading.store(true);

    // Probe duration via libav synchronously (fast — no decode).
    // This runs on the main thread before the background thread starts,
    // so there is no concurrent libav usage with video.cpp.
    {
        AVFormatContext* fc = nullptr;
        if (avformat_open_input(&fc, path.c_str(), nullptr, nullptr) == 0) {
            avformat_find_stream_info(fc, nullptr);
            if (fc->duration != AV_NOPTS_VALUE)
                g_duration = (float)fc->duration / (float)AV_TIME_BASE;
            avformat_close_input(&fc);
        }
    }

    // Background thread: fork ffmpeg to decode audio to raw f32le PCM.
    // No libav calls in this thread — avoids concurrent libav with video.cpp.
    std::thread([path]() {
        static const char* TMP = "/tmp/pms_audio_decode.raw";

        // ffmpeg -i INPUT -vn -ar 44100 -ac 2 -f f32le TMP
        const char* args[] = {
            "ffmpeg", "-hide_banner", "-loglevel", "error",
            "-y", "-i", path.c_str(),
            "-vn", "-ar", "44100", "-ac", "2", "-f", "f32le", TMP,
            nullptr
        };

        pid_t pid = fork();
        if (pid == 0) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
            execvp("ffmpeg", const_cast<char**>(args));
            _exit(127);
        }
        if (pid < 0) { g_loading.store(false); return; }

        int wstat = 0;
        waitpid(pid, &wstat, 0);
        if (!WIFEXITED(wstat) || WEXITSTATUS(wstat) != 0) {
            g_loading.store(false); return;
        }

        // Read raw PCM into g_samples
        FILE* f = fopen(TMP, "rb");
        if (!f) { g_loading.store(false); return; }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        rewind(f);
        if (sz > 0) {
            g_samples.resize((size_t)sz / sizeof(float));
            fread(g_samples.data(), sizeof(float), g_samples.size(), f);
        }
        fclose(f);

        g_duration = (float)g_samples.size() / 2.f / 44100.f;
        g_read_pos = 0;

        // Init miniaudio device now that samples are ready.
        ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
        cfg.playback.format   = ma_format_f32;
        cfg.playback.channels = 2;
        cfg.sampleRate        = 44100;
        cfg.dataCallback      = data_callback;

        if (ma_device_init(nullptr, &cfg, &g_device) == MA_SUCCESS) {
            g_device_cfg  = cfg;
            g_device_init = true;
        }

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
    g_read_pos = (sample < g_samples.size()) ? sample : g_samples.size();
}

float audio_duration()   { return g_duration; }
bool  audio_is_playing() { return g_device_init && ma_device_is_started(&g_device); }

float audio_position() {
    return (float)(g_read_pos / 2) / 44100.f;
}

bool audio_probe(const std::string& path, AudioMeta& meta) {
    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, path.c_str(), nullptr, nullptr) < 0) return false;
    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        avformat_close_input(&fmt_ctx); return false;
    }
    meta.duration_secs = (float)fmt_ctx->duration / (float)AV_TIME_BASE;
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
