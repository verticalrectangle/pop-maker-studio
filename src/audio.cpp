#include "audio.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <vector>
#include <thread>
#include <atomic>
#include <cstring>
#include <cstdio>

// ── PCM decode via FFmpeg → miniaudio playback ────────────────────────────────

static ma_device        g_device;
static ma_device_config g_device_cfg;
static bool             g_device_init = false;

static std::vector<float> g_samples;   // interleaved stereo f32
static size_t             g_read_pos  = 0;
static int                g_sample_rate = 44100;
static float              g_duration   = 0.f;
static float              g_volume     = 1.f;

// Set to true while background decode is running.
// audio_shutdown() is called before the thread starts, so data_callback
// is NOT running during the decode — no mutex is needed.
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
    // Stop device on main thread — after this returns, data_callback is dead.
    audio_shutdown();
    g_samples.clear();
    g_read_pos = 0;
    g_duration = 0.f;
    g_loading.store(true);

    // Probe duration synchronously so state.duration is set before we return.
    {
        AVFormatContext* fc = nullptr;
        if (avformat_open_input(&fc, path.c_str(), nullptr, nullptr) == 0) {
            avformat_find_stream_info(fc, nullptr);
            if (fc->duration != AV_NOPTS_VALUE)
                g_duration = (float)fc->duration / (float)AV_TIME_BASE;
            avformat_close_input(&fc);
        }
    }

    std::thread([path]() {
        AVFormatContext* fmt_ctx = nullptr;
        if (avformat_open_input(&fmt_ctx, path.c_str(), nullptr, nullptr) < 0) {
            g_loading.store(false); return;
        }
        if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
            avformat_close_input(&fmt_ctx); g_loading.store(false); return;
        }

        int audio_stream = -1;
        for (unsigned i = 0; i < fmt_ctx->nb_streams; ++i) {
            if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                audio_stream = (int)i; break;
            }
        }
        if (audio_stream < 0) {
            avformat_close_input(&fmt_ctx); g_loading.store(false); return;
        }

        const AVCodec* codec = avcodec_find_decoder(
            fmt_ctx->streams[audio_stream]->codecpar->codec_id);
        AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(codec_ctx, fmt_ctx->streams[audio_stream]->codecpar);
        avcodec_open2(codec_ctx, codec, nullptr);

        int sample_rate = codec_ctx->sample_rate;

        SwrContext* swr = nullptr;
        AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
        swr_alloc_set_opts2(&swr,
            &out_layout, AV_SAMPLE_FMT_FLT, sample_rate,
            &codec_ctx->ch_layout, codec_ctx->sample_fmt, codec_ctx->sample_rate,
            0, nullptr);
        swr_init(swr);

        AVPacket* pkt = av_packet_alloc();
        AVFrame*  frm = av_frame_alloc();

        std::vector<float> tmp;
        while (av_read_frame(fmt_ctx, pkt) >= 0) {
            if (pkt->stream_index != audio_stream) { av_packet_unref(pkt); continue; }
            avcodec_send_packet(codec_ctx, pkt);
            while (avcodec_receive_frame(codec_ctx, frm) == 0) {
                int out_samples = av_rescale_rnd(
                    swr_get_delay(swr, codec_ctx->sample_rate) + frm->nb_samples,
                    sample_rate, codec_ctx->sample_rate, AV_ROUND_UP);
                std::vector<float> chunk((size_t)out_samples * 2);
                uint8_t* out_ptr = (uint8_t*)chunk.data();
                int converted = swr_convert(swr, &out_ptr, out_samples,
                    (const uint8_t**)frm->data, frm->nb_samples);
                if (converted > 0)
                    tmp.insert(tmp.end(), chunk.begin(),
                               chunk.begin() + (size_t)converted * 2);
                av_frame_unref(frm);
            }
            av_packet_unref(pkt);
        }

        av_frame_free(&frm);
        av_packet_free(&pkt);
        swr_free(&swr);
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);

        // Device is stopped; no callback is running — safe to swap directly.
        g_samples     = std::move(tmp);
        g_sample_rate = sample_rate;
        g_duration    = (float)g_samples.size() / 2.f / (float)sample_rate;
        g_read_pos    = 0;

        // Init miniaudio device on the background thread.
        // miniaudio is safe to init from any thread.
        ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
        cfg.playback.format   = ma_format_f32;
        cfg.playback.channels = 2;
        cfg.sampleRate        = (ma_uint32)sample_rate;
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
    if (g_loading.load()) return;  // decode in progress, device not ready
    size_t sample = (size_t)(seconds * (float)g_sample_rate) * 2;
    g_read_pos = (sample < g_samples.size()) ? sample : g_samples.size();
}

float audio_duration()   { return g_duration; }
bool  audio_is_playing() { return g_device_init && ma_device_is_started(&g_device); }

float audio_position() {
    if (g_sample_rate == 0) return 0.f;
    return (float)(g_read_pos / 2) / (float)g_sample_rate;
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
