#include "beat_detect.h"
#include <cmath>
#include <cstring>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <aubio/aubio.h>
}

static const uint_t AUBIO_SR      = 44100;
static const uint_t AUBIO_HOP     = 512;
static const uint_t AUBIO_WIN     = 1024;

BeatResult beat_detect(const std::string& path) {
    BeatResult result;
    result.source_id = path;

    // ── Decode audio to mono f32 at AUBIO_SR ─────────────────────────────────
    std::vector<float> pcm;

    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, path.c_str(), nullptr, nullptr) < 0)
        return result;
    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        avformat_close_input(&fmt_ctx); return result;
    }

    int audio_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audio_idx < 0) { avformat_close_input(&fmt_ctx); return result; }

    AVStream*       stream    = fmt_ctx->streams[audio_idx];
    const AVCodec*  codec     = avcodec_find_decoder(stream->codecpar->codec_id);
    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codec_ctx, stream->codecpar);
    if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return result;
    }

    SwrContext* swr = swr_alloc();
#if LIBAVUTIL_VERSION_MAJOR >= 57
    av_opt_set_chlayout  (swr, "in_chlayout",   &codec_ctx->ch_layout, 0);
    av_opt_set_int       (swr, "in_sample_rate",  codec_ctx->sample_rate, 0);
    av_opt_set_sample_fmt(swr, "in_sample_fmt",   codec_ctx->sample_fmt, 0);
    AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
    av_opt_set_chlayout  (swr, "out_chlayout",   &mono, 0);
#else
    av_opt_set_int       (swr, "in_channel_count",   codec_ctx->channels, 0);
    av_opt_set_int       (swr, "in_channel_layout",  (int64_t)codec_ctx->channel_layout, 0);
    av_opt_set_int       (swr, "in_sample_rate",     codec_ctx->sample_rate, 0);
    av_opt_set_sample_fmt(swr, "in_sample_fmt",      codec_ctx->sample_fmt, 0);
    av_opt_set_int       (swr, "out_channel_count",  1, 0);
    av_opt_set_int       (swr, "out_channel_layout", AV_CH_LAYOUT_MONO, 0);
#endif
    av_opt_set_int       (swr, "out_sample_rate", (int)AUBIO_SR, 0);
    av_opt_set_sample_fmt(swr, "out_sample_fmt",  AV_SAMPLE_FMT_FLT, 0);
    swr_init(swr);

    AVPacket* pkt   = av_packet_alloc();
    AVFrame*  frame = av_frame_alloc();

    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == audio_idx) {
            avcodec_send_packet(codec_ctx, pkt);
            while (avcodec_receive_frame(codec_ctx, frame) == 0) {
                int out_samples = (int)av_rescale_rnd(
                    swr_get_delay(swr, codec_ctx->sample_rate) + frame->nb_samples,
                    (int)AUBIO_SR, codec_ctx->sample_rate, AV_ROUND_UP);
                std::vector<float> buf(out_samples);
                uint8_t* out_ptr = (uint8_t*)buf.data();
                int got = swr_convert(swr, &out_ptr, out_samples,
                                      (const uint8_t**)frame->data, frame->nb_samples);
                if (got > 0) pcm.insert(pcm.end(), buf.begin(), buf.begin() + got);
            }
        }
        av_packet_unref(pkt);
    }
    // flush swr
    {
        int tail = (int)av_rescale_rnd(swr_get_delay(swr, codec_ctx->sample_rate),
                                        (int)AUBIO_SR, codec_ctx->sample_rate, AV_ROUND_UP);
        if (tail > 0) {
            std::vector<float> buf(tail);
            uint8_t* out_ptr = (uint8_t*)buf.data();
            int got = swr_convert(swr, &out_ptr, tail, nullptr, 0);
            if (got > 0) pcm.insert(pcm.end(), buf.begin(), buf.begin() + got);
        }
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
    swr_free(&swr);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);

    if (pcm.empty()) return result;

    // ── Per-second RMS energy (normalised 0–1) ────────────────────────────────
    result.duration = (float)pcm.size() / (float)AUBIO_SR;
    {
        int nsec = (int)std::ceil(result.duration);
        result.rms.resize((size_t)nsec, 0.f);
        float peak = 0.f;
        for (int s = 0; s < nsec; ++s) {
            size_t i0 = (size_t)s * AUBIO_SR;
            size_t i1 = std::min(i0 + AUBIO_SR, pcm.size());
            float sq = 0.f;
            for (size_t i = i0; i < i1; ++i) sq += pcm[i] * pcm[i];
            float r = std::sqrt(sq / (float)(i1 - i0));
            result.rms[s] = r;
            if (r > peak) peak = r;
        }
        if (peak > 0.f)
            for (auto& r : result.rms) r /= peak;
    }

    // ── Run aubio beat tracker ────────────────────────────────────────────────
    aubio_tempo_t* tempo = new_aubio_tempo("default", AUBIO_WIN, AUBIO_HOP, AUBIO_SR);
    if (!tempo) return result;

    fvec_t* in_buf  = new_fvec(AUBIO_HOP);
    fvec_t* out_buf = new_fvec(2);

    size_t pos = 0;
    while (pos + AUBIO_HOP <= pcm.size()) {
        memcpy(in_buf->data, pcm.data() + pos, AUBIO_HOP * sizeof(float));
        aubio_tempo_do(tempo, in_buf, out_buf);
        if (out_buf->data[0] != 0.f) {
            float t = aubio_tempo_get_last_s(tempo);
            if (t >= 0.f) result.beats.push_back(t);
        }
        pos += AUBIO_HOP;
    }

    float bpm = aubio_tempo_get_bpm(tempo);

    // Resolve half/double tempo: prefer range 80–180 BPM
    if (bpm > 0.f && bpm < 80.f)  bpm *= 2.f;
    if (bpm > 0.f && bpm > 180.f) bpm *= 0.5f;

    result.bpm = bpm;
    result.ok  = bpm > 0.f;

    del_fvec(out_buf);
    del_fvec(in_buf);
    del_aubio_tempo(tempo);

    return result;
}
