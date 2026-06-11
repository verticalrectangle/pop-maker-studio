#include "scene_detect.h"
#include "stb_image_write.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <filesystem>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <vector>
#include <string>

namespace fs = std::filesystem;

// ── Constants ────────────────────────────────────────────────────────────────

static const int   THUMB_W      = 64;
static const int   THUMB_H      = 36;
static const float SCENE_THRESH = 0.10f;  // MAD as fraction of 255
static const float MIN_GAP_SECS = 2.0f;
static const int   JPEG_QUALITY = 85;
static const int   JPEG_LONG    = 640;    // longest edge

// ── Internal helpers ──────────────────────────────────────────────────────────

static float thumb_mad(const uint8_t* a, const uint8_t* b) {
    int64_t sum = 0;
    for (int i = 0; i < THUMB_W * THUMB_H; i++)
        sum += std::abs((int)a[i] - (int)b[i]);
    return (float)sum / (float)(THUMB_W * THUMB_H * 255);
}

static bool write_frame_jpeg(AVFrame* frame, AVPixelFormat fmt,
                              int src_w, int src_h,
                              const std::string& path) {
    int dw = src_w, dh = src_h;
    int longest = std::max(src_w, src_h);
    if (longest > JPEG_LONG) {
        float s = (float)JPEG_LONG / longest;
        dw = std::max(1, (int)(src_w * s));
        dh = std::max(1, (int)(src_h * s));
    }

    SwsContext* sws = sws_getContext(src_w, src_h, fmt,
                                     dw, dh, AV_PIX_FMT_RGB24,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) return false;

    std::vector<uint8_t> rgb(dw * dh * 3);
    uint8_t* dst[1]  = { rgb.data() };
    int      dls[1]  = { dw * 3 };
    sws_scale(sws, frame->data, frame->linesize, 0, src_h, dst, dls);
    sws_freeContext(sws);

    return stbi_write_jpg(path.c_str(), dw, dh, 3, rgb.data(), JPEG_QUALITY) != 0;
}

// ── Public API ────────────────────────────────────────────────────────────────

std::vector<KeyFrame> extract_keyframes(const std::string& video_path,
                                        int   max_frames,
                                        bool* capped) {
    if (capped) *capped = false;

    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, video_path.c_str(), nullptr, nullptr) < 0)
        return {};
    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        avformat_close_input(&fmt_ctx); return {};
    }

    int vi = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vi < 0) { avformat_close_input(&fmt_ctx); return {}; }

    AVStream*       stream = fmt_ctx->streams[vi];
    const AVCodec*  codec  = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) { avformat_close_input(&fmt_ctx); return {}; }

    AVCodecContext* cc = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(cc, stream->codecpar);
    cc->thread_count = 2;
    if (avcodec_open2(cc, codec, nullptr) < 0) {
        avcodec_free_context(&cc); avformat_close_input(&fmt_ctx); return {};
    }

    int   src_w = cc->width, src_h = cc->height;
    double fps  = av_q2d(stream->avg_frame_rate);
    if (fps <= 0 || fps > 300) fps = 30.0;

    // Probe every ~0.5s
    int probe_step = std::max(1, (int)(fps * 0.5));

    SwsContext* sws_thumb = sws_getContext(src_w, src_h, cc->pix_fmt,
                                           THUMB_W, THUMB_H, AV_PIX_FMT_GRAY8,
                                           SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_thumb) {
        avcodec_free_context(&cc); avformat_close_input(&fmt_ctx); return {};
    }

    std::string frames_dir = video_path + ".pms_frames";
    fs::create_directories(frames_dir);

    // ── Pass 1: detect scenes and save all JPEG candidates ────────────────────

    struct Candidate { float ts; std::string path; };
    std::vector<Candidate> candidates;

    AVPacket* pkt   = av_packet_alloc();
    AVFrame*  frame = av_frame_alloc();

    std::vector<uint8_t> cur_thumb(THUMB_W * THUMB_H);
    std::vector<uint8_t> prev_thumb(THUMB_W * THUMB_H, 128);
    uint8_t* tdata[1] = { cur_thumb.data() };
    int      tls[1]   = { THUMB_W };

    bool  first     = true;
    float last_ts   = -MIN_GAP_SECS;
    int   fidx      = 0;

    auto flush_decoder = [&] {
        avcodec_send_packet(cc, nullptr);
        while (avcodec_receive_frame(cc, frame) == 0) { /* drain */ }
    };

    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index != vi) { av_packet_unref(pkt); continue; }
        avcodec_send_packet(cc, pkt);
        av_packet_unref(pkt);

        while (avcodec_receive_frame(cc, frame) == 0) {
            int cur = fidx++;
            if (cur % probe_step != 0) continue;

            int64_t pts = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                          ? frame->best_effort_timestamp : frame->pts;
            float ts = (pts != AV_NOPTS_VALUE)
                       ? (float)(pts * av_q2d(stream->time_base)) : 0.f;

            sws_scale(sws_thumb, frame->data, frame->linesize, 0, src_h,
                      tdata, tls);
            float score = thumb_mad(cur_thumb.data(), prev_thumb.data());
            memcpy(prev_thumb.data(), cur_thumb.data(), THUMB_W * THUMB_H);

            bool pick = first || (score >= SCENE_THRESH && ts - last_ts >= MIN_GAP_SECS);
            if (!pick) continue;
            first   = false;
            last_ts = ts;

            char fname[32];
            snprintf(fname, sizeof(fname), "%04d.jpg", (int)candidates.size() + 1);
            std::string path = frames_dir + "/" + fname;
            if (write_frame_jpeg(frame, cc->pix_fmt, src_w, src_h, path))
                candidates.push_back({ts, path});
        }
    }
    flush_decoder();

    av_frame_free(&frame);
    av_packet_free(&pkt);
    sws_freeContext(sws_thumb);
    avcodec_free_context(&cc);
    avformat_close_input(&fmt_ctx);

    if (candidates.empty()) return {};

    // ── Pass 2: apply cap, clean up un-selected files ─────────────────────────

    std::vector<int> keep_idx;  // indices into candidates to keep
    if ((int)candidates.size() <= max_frames) {
        for (int i = 0; i < (int)candidates.size(); i++) keep_idx.push_back(i);
    } else {
        if (capped) *capped = true;
        for (int j = 0; j < max_frames; j++) {
            int i = (int)((double)j * (candidates.size() - 1) / (max_frames - 1) + 0.5);
            keep_idx.push_back(std::min(i, (int)candidates.size() - 1));
        }
    }

    // Delete files not in keep_idx
    for (int i = 0; i < (int)candidates.size(); i++) {
        bool keep = std::find(keep_idx.begin(), keep_idx.end(), i) != keep_idx.end();
        if (!keep) fs::remove(candidates[i].path);
    }

    // Build result
    std::vector<KeyFrame> result;
    result.reserve(keep_idx.size());
    for (int idx : keep_idx)
        result.push_back({candidates[idx].ts, candidates[idx].path});

    return result;
}

std::vector<KeyFrame> extract_frames_at(const std::string& video_path,
                                        const std::vector<float>& times) {
    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, video_path.c_str(), nullptr, nullptr) < 0)
        return {};
    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        avformat_close_input(&fmt_ctx); return {};
    }
    int vi = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vi < 0) { avformat_close_input(&fmt_ctx); return {}; }

    AVStream*      stream = fmt_ctx->streams[vi];
    const AVCodec* codec  = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) { avformat_close_input(&fmt_ctx); return {}; }

    AVCodecContext* cc = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(cc, stream->codecpar);
    cc->thread_count = 2;
    if (avcodec_open2(cc, codec, nullptr) < 0) {
        avcodec_free_context(&cc); avformat_close_input(&fmt_ctx); return {};
    }

    std::string frames_dir = video_path + ".pms_frames";
    fs::create_directories(frames_dir);

    AVPacket* pkt = av_packet_alloc();
    AVFrame*  frm = av_frame_alloc();
    double    tb  = av_q2d(stream->time_base);

    std::vector<KeyFrame> out;
    for (float t : times) {
        int64_t seek_ts = (int64_t)((double)t / tb);
        if (av_seek_frame(fmt_ctx, vi, seek_ts, AVSEEK_FLAG_BACKWARD) < 0)
            continue;
        avcodec_flush_buffers(cc);

        // Decode forward from the preceding keyframe until we reach t.
        bool got = false;
        while (!got && av_read_frame(fmt_ctx, pkt) >= 0) {
            if (pkt->stream_index != vi) { av_packet_unref(pkt); continue; }
            if (avcodec_send_packet(cc, pkt) == 0) {
                while (avcodec_receive_frame(cc, frm) == 0) {
                    double pts = (frm->best_effort_timestamp == AV_NOPTS_VALUE)
                                     ? t
                                     : frm->best_effort_timestamp * tb;
                    if (pts + 1e-3 >= (double)t) {
                        char name[48];
                        snprintf(name, sizeof(name), "/at_%d_ms.jpg",
                                 (int)(t * 1000.f));
                        std::string jp = frames_dir + name;
                        if (write_frame_jpeg(frm, (AVPixelFormat)frm->format,
                                             cc->width, cc->height, jp))
                            out.push_back({t, jp});
                        got = true;
                        break;
                    }
                }
            }
            av_packet_unref(pkt);
        }
        // EOF before reaching t (time past the end) → skipped.
    }

    av_frame_free(&frm);
    av_packet_free(&pkt);
    avcodec_free_context(&cc);
    avformat_close_input(&fmt_ctx);
    return out;
}
