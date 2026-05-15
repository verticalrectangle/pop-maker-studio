#include "waveform.h"
#include <unordered_map>
#include <mutex>
#include <thread>
#include <cmath>
#include <cstring>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

static std::unordered_map<std::string, WaveformData> s_cache;
static std::mutex s_mutex;

static void decode_waveform(const std::string path) {
    std::vector<float> samples;

    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, path.c_str(), nullptr, nullptr) < 0) goto done;
    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) goto close_fmt;

    {
        int audio_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (audio_idx < 0) goto close_fmt;

        AVStream* stream = fmt_ctx->streams[audio_idx];
        const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!codec) goto close_fmt;

        AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(codec_ctx, stream->codecpar);
        if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
            avcodec_free_context(&codec_ctx);
            goto close_fmt;
        }

        // Resample to mono f32 at WAVEFORM_FPS * hop_size
        // We'll collect mono f32 samples then downsample to WAVEFORM_FPS RMS values.
        const int out_sr = 22050;
        const int hop    = (int)(out_sr / WAVEFORM_FPS);

        SwrContext* swr = swr_alloc();
#if LIBAVUTIL_VERSION_MAJOR >= 57
        av_opt_set_chlayout(swr,  "in_chlayout",  &codec_ctx->ch_layout, 0);
        av_opt_set_int(swr, "in_sample_rate",   codec_ctx->sample_rate, 0);
        av_opt_set_sample_fmt(swr, "in_sample_fmt", codec_ctx->sample_fmt, 0);
        AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
        av_opt_set_chlayout(swr,  "out_chlayout",  &mono, 0);
#else
        av_opt_set_int(swr, "in_channel_count",   codec_ctx->channels, 0);
        av_opt_set_int(swr, "in_channel_layout",  (int64_t)codec_ctx->channel_layout, 0);
        av_opt_set_int(swr, "in_sample_rate",     codec_ctx->sample_rate, 0);
        av_opt_set_sample_fmt(swr, "in_sample_fmt", codec_ctx->sample_fmt, 0);
        av_opt_set_int(swr, "out_channel_count",  1, 0);
        av_opt_set_int(swr, "out_channel_layout", AV_CH_LAYOUT_MONO, 0);
#endif
        av_opt_set_int(swr, "out_sample_rate",  out_sr, 0);
        av_opt_set_sample_fmt(swr, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);
        swr_init(swr);

        std::vector<float> pcm;
        AVPacket* pkt  = av_packet_alloc();
        AVFrame*  frame = av_frame_alloc();

        while (av_read_frame(fmt_ctx, pkt) >= 0) {
            if (pkt->stream_index == audio_idx) {
                avcodec_send_packet(codec_ctx, pkt);
                while (avcodec_receive_frame(codec_ctx, frame) == 0) {
                    int out_samples = (int)av_rescale_rnd(
                        swr_get_delay(swr, codec_ctx->sample_rate) + frame->nb_samples,
                        out_sr, codec_ctx->sample_rate, AV_ROUND_UP);
                    std::vector<float> buf(out_samples);
                    uint8_t* out_ptr = (uint8_t*)buf.data();
                    int got = swr_convert(swr, &out_ptr, out_samples,
                                         (const uint8_t**)frame->data, frame->nb_samples);
                    if (got > 0) {
                        pcm.insert(pcm.end(), buf.begin(), buf.begin() + got);
                    }
                }
            }
            av_packet_unref(pkt);
        }

        // Flush
        {
            int out_samples = (int)av_rescale_rnd(swr_get_delay(swr, codec_ctx->sample_rate),
                                                   out_sr, codec_ctx->sample_rate, AV_ROUND_UP);
            if (out_samples > 0) {
                std::vector<float> buf(out_samples);
                uint8_t* out_ptr = (uint8_t*)buf.data();
                int got = swr_convert(swr, &out_ptr, out_samples, nullptr, 0);
                if (got > 0) pcm.insert(pcm.end(), buf.begin(), buf.begin() + got);
            }
        }

        av_packet_free(&pkt);
        av_frame_free(&frame);
        swr_free(&swr);
        avcodec_free_context(&codec_ctx);

        // Compute RMS per hop
        int n_frames = (int)(pcm.size() / hop);
        samples.reserve(n_frames);
        float peak = 1e-6f;
        for (int fi = 0; fi < n_frames; ++fi) {
            float sum = 0.f;
            for (int i = 0; i < hop; ++i) {
                float s = pcm[fi * hop + i];
                sum += s * s;
            }
            float rms = sqrtf(sum / hop);
            if (rms > peak) peak = rms;
            samples.push_back(rms);
        }
        // Normalize
        for (auto& v : samples) v /= peak;
    }

close_fmt:
    avformat_close_input(&fmt_ctx);
done:
    std::lock_guard<std::mutex> lk(s_mutex);
    auto& wd = s_cache[path];
    wd.samples = std::move(samples);
    wd.ready   = true;
    wd.loading = false;
}

const WaveformData* waveform_get(const std::string& path) {
    std::lock_guard<std::mutex> lk(s_mutex);
    auto it = s_cache.find(path);
    if (it != s_cache.end()) {
        return it->second.ready ? &it->second : nullptr;
    }
    // Insert placeholder and kick off decode thread
    auto& wd  = s_cache[path];
    wd.loading = true;
    std::thread([path]() { decode_waveform(path); }).detach();
    return nullptr;
}

void waveform_clear_cache() {
    std::lock_guard<std::mutex> lk(s_mutex);
    s_cache.clear();
}
