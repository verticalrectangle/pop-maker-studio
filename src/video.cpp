#include "video.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <GL/gl.h>
#include <thread>
#include <mutex>
#include <atomic>
#include <deque>
#include <cstring>
#include <cstdlib>

// ── Internal state ────────────────────────────────────────────────────────────

static struct VideoState {
    AVFormatContext* fmt_ctx    = nullptr;
    AVCodecContext*  codec_ctx  = nullptr;
    SwsContext*      sws        = nullptr;
    int              stream_idx = -1;
    VideoInfo        info       = {};

    // Decode thread
    std::thread        decode_thread;
    std::atomic<bool>  running{false};
    std::atomic<bool>  seek_requested{false};
    std::atomic<double> seek_target{0.0};

    // Frame ring — mutex protected
    std::mutex              frame_mutex;
    std::deque<VideoFrame*> frame_queue;  // decoded frames, PTS order
    VideoFrame*             current = nullptr;

    // OpenGL texture
    GLuint tex_id = 0;
} g_vs;

static constexpr int MAX_QUEUE = 8;

static void free_frame(VideoFrame* f) {
    if (!f) return;
    free(f->data);
    delete f;
}

static void decode_thread_fn() {
    AVPacket* pkt = av_packet_alloc();
    AVFrame*  frm = av_frame_alloc();

    while (g_vs.running.load()) {
        // Handle seek
        if (g_vs.seek_requested.load()) {
            g_vs.seek_requested.store(false);
            double target = g_vs.seek_target.load();
            int64_t ts = (int64_t)(target * AV_TIME_BASE);
            av_seek_frame(g_vs.fmt_ctx, -1, ts, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(g_vs.codec_ctx);
            std::lock_guard<std::mutex> lock(g_vs.frame_mutex);
            for (auto* f : g_vs.frame_queue) free_frame(f);
            g_vs.frame_queue.clear();
        }

        // Throttle if queue is full
        {
            std::lock_guard<std::mutex> lock(g_vs.frame_mutex);
            if ((int)g_vs.frame_queue.size() >= MAX_QUEUE) {
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
                continue;
            }
        }

        int ret = av_read_frame(g_vs.fmt_ctx, pkt);
        if (ret == AVERROR_EOF) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        if (ret < 0) break;

        if (pkt->stream_index != g_vs.stream_idx) {
            av_packet_unref(pkt);
            continue;
        }

        avcodec_send_packet(g_vs.codec_ctx, pkt);
        av_packet_unref(pkt);

        while (avcodec_receive_frame(g_vs.codec_ctx, frm) == 0) {
            // Convert to RGBA
            auto* vf   = new VideoFrame();
            vf->width  = g_vs.info.width;
            vf->height = g_vs.info.height;
            vf->data   = (uint8_t*)malloc(vf->width * vf->height * 4);

            AVStream* st = g_vs.fmt_ctx->streams[g_vs.stream_idx];
            vf->pts = frm->pts * av_q2d(st->time_base);

            uint8_t* dst[1]  = { vf->data };
            int      lsz[1]  = { vf->width * 4 };
            sws_scale(g_vs.sws,
                (const uint8_t* const*)frm->data, frm->linesize,
                0, frm->height, dst, lsz);

            av_frame_unref(frm);

            std::lock_guard<std::mutex> lock(g_vs.frame_mutex);
            g_vs.frame_queue.push_back(vf);
        }
    }

    av_packet_free(&pkt);
    av_frame_free(&frm);
}

// ── Public API ────────────────────────────────────────────────────────────────

bool video_open(const std::string& path) {
    video_close();

    if (avformat_open_input(&g_vs.fmt_ctx, path.c_str(), nullptr, nullptr) < 0)
        return false;
    if (avformat_find_stream_info(g_vs.fmt_ctx, nullptr) < 0) {
        avformat_close_input(&g_vs.fmt_ctx);
        return false;
    }

    for (unsigned i = 0; i < g_vs.fmt_ctx->nb_streams; ++i) {
        if (g_vs.fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            g_vs.stream_idx = (int)i;
            break;
        }
    }
    if (g_vs.stream_idx < 0) { avformat_close_input(&g_vs.fmt_ctx); return false; }

    AVStream* st = g_vs.fmt_ctx->streams[g_vs.stream_idx];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    g_vs.codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(g_vs.codec_ctx, st->codecpar);
    avcodec_open2(g_vs.codec_ctx, codec, nullptr);

    g_vs.info.width    = g_vs.codec_ctx->width;
    g_vs.info.height   = g_vs.codec_ctx->height;
    g_vs.info.duration = (double)g_vs.fmt_ctx->duration / AV_TIME_BASE;
    g_vs.info.fps      = av_q2d(st->avg_frame_rate);

    g_vs.sws = sws_getContext(
        g_vs.info.width, g_vs.info.height, g_vs.codec_ctx->pix_fmt,
        g_vs.info.width, g_vs.info.height, AV_PIX_FMT_RGBA,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    g_vs.running.store(true);
    g_vs.decode_thread = std::thread(decode_thread_fn);
    return true;
}

void video_close() {
    g_vs.running.store(false);
    if (g_vs.decode_thread.joinable()) g_vs.decode_thread.join();

    std::lock_guard<std::mutex> lock(g_vs.frame_mutex);
    for (auto* f : g_vs.frame_queue) free_frame(f);
    g_vs.frame_queue.clear();
    free_frame(g_vs.current); g_vs.current = nullptr;

    if (g_vs.sws)       { sws_freeContext(g_vs.sws);       g_vs.sws = nullptr; }
    if (g_vs.codec_ctx) { avcodec_free_context(&g_vs.codec_ctx); }
    if (g_vs.fmt_ctx)   { avformat_close_input(&g_vs.fmt_ctx); }
    if (g_vs.tex_id)    { glDeleteTextures(1, &g_vs.tex_id); g_vs.tex_id = 0; }
    g_vs.stream_idx = -1;
}

bool      video_is_open() { return g_vs.stream_idx >= 0; }
VideoInfo video_info()    { return g_vs.info; }

void video_seek(double seconds) {
    g_vs.seek_target.store(seconds);
    g_vs.seek_requested.store(true);
}

const VideoFrame* video_get_frame(double playhead) {
    std::lock_guard<std::mutex> lock(g_vs.frame_mutex);

    // Drain frames that are behind the playhead, keep the last valid one
    while (g_vs.frame_queue.size() > 1 &&
           g_vs.frame_queue.front()->pts <= playhead) {
        free_frame(g_vs.current);
        g_vs.current = g_vs.frame_queue.front();
        g_vs.frame_queue.pop_front();
    }
    return g_vs.current;
}

uintptr_t video_get_texture(double playhead) {
    const VideoFrame* f = video_get_frame(playhead);
    if (!f) return 0;

    if (g_vs.tex_id == 0) {
        glGenTextures(1, &g_vs.tex_id);
        glBindTexture(GL_TEXTURE_2D, g_vs.tex_id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, f->width, f->height,
                     0, GL_RGBA, GL_UNSIGNED_BYTE, f->data);
    } else {
        glBindTexture(GL_TEXTURE_2D, g_vs.tex_id);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, f->width, f->height,
                        GL_RGBA, GL_UNSIGNED_BYTE, f->data);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    return (uintptr_t)g_vs.tex_id;
}

VideoFrame* video_decode_frame_at(double seconds) {
    // Blocking single-frame decode for export — opens a fresh context
    AVFormatContext* fmt = nullptr;
    // (reuse the already-open format context path from g_vs)
    if (!g_vs.fmt_ctx) return nullptr;

    int64_t ts = (int64_t)(seconds * AV_TIME_BASE);
    av_seek_frame(g_vs.fmt_ctx, -1, ts, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(g_vs.codec_ctx);

    AVPacket* pkt = av_packet_alloc();
    AVFrame*  frm = av_frame_alloc();
    VideoFrame* result = nullptr;

    while (av_read_frame(g_vs.fmt_ctx, pkt) >= 0 && !result) {
        if (pkt->stream_index != g_vs.stream_idx) { av_packet_unref(pkt); continue; }
        avcodec_send_packet(g_vs.codec_ctx, pkt);
        av_packet_unref(pkt);
        while (avcodec_receive_frame(g_vs.codec_ctx, frm) == 0 && !result) {
            result = new VideoFrame();
            result->width  = g_vs.info.width;
            result->height = g_vs.info.height;
            result->data   = (uint8_t*)malloc(result->width * result->height * 4);
            uint8_t* dst[1] = { result->data };
            int      lsz[1] = { result->width * 4 };
            sws_scale(g_vs.sws,
                (const uint8_t* const*)frm->data, frm->linesize,
                0, frm->height, dst, lsz);
            av_frame_unref(frm);
        }
    }

    av_packet_free(&pkt);
    av_frame_free(&frm);
    return result;
}
