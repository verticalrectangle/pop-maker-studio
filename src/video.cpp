#include "video.h"

// ── stb_image — JPEG decode for preview frames ────────────────────────────────
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_image.h"
#pragma GCC diagnostic pop

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <GL/gl.h>

#include <cstdio>
#include <cstring>
#include <vector>

// ── Preview state ─────────────────────────────────────────────────────────────

struct PreviewState {
    FILE*     mjpeg_file     = nullptr;
    ProxyInfo proxy          = {};
    int       last_frame_idx = -1;
    GLuint    tex    = 0;
    int       tex_w  = 0;
    int       tex_h  = 0;
    bool      is_proxy = false;
    bool      is_open  = false;
    VideoInfo info = {};
};

static PreviewState g_pv[MAX_VIDEO_TRACKS];

// Separate texture for hover-preview thumbnails (track 0's proxy).
static struct ThumbState {
    GLuint tex            = 0;
    int    tex_w          = 0;
    int    tex_h          = 0;
    int    last_frame_idx = -1;
} g_th;

// Upload a JPEG buffer into a GL texture slot.
static void upload_jpeg(GLuint* tex, int* tex_w, int* tex_h,
                        const uint8_t* buf, size_t sz) {
    int w, h, ch;
    uint8_t* pixels = stbi_load_from_memory(buf, (int)sz, &w, &h, &ch, 3);
    if (!pixels) return;

    if (*tex == 0) {
        glGenTextures(1, tex);
        glBindTexture(GL_TEXTURE_2D, *tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, *tex);
    }

    if (w != *tex_w || h != *tex_h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0,
                     GL_RGB, GL_UNSIGNED_BYTE, pixels);
        *tex_w = w; *tex_h = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                        GL_RGB, GL_UNSIGNED_BYTE, pixels);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(pixels);
}

// ── Internal: read one JPEG frame from a proxy at frame_idx ──────────────────

static uintptr_t decode_proxy_frame(PreviewState& pv, int frame_idx) {
    uint64_t offset = pv.proxy.offsets[(size_t)frame_idx];
    bool is_last = ((size_t)frame_idx + 1 >= pv.proxy.offsets.size());

    size_t frame_sz = 0;
    if (!is_last) {
        frame_sz = (size_t)(pv.proxy.offsets[(size_t)frame_idx + 1] - offset);
    } else {
        fseeko(pv.mjpeg_file, (off_t)offset, SEEK_SET);
        long cur = ftell(pv.mjpeg_file);
        fseeko(pv.mjpeg_file, 0, SEEK_END);
        long end = ftell(pv.mjpeg_file);
        frame_sz = (end > cur) ? (size_t)(end - cur) : 0;
    }
    if (frame_sz == 0) return pv.tex ? (uintptr_t)pv.tex : 0;

    static std::vector<uint8_t> s_buf;
    s_buf.resize(frame_sz);
    fseeko(pv.mjpeg_file, (off_t)offset, SEEK_SET);
    size_t got = fread(s_buf.data(), 1, frame_sz, pv.mjpeg_file);
    if (got == 0) return pv.tex ? (uintptr_t)pv.tex : 0;

    upload_jpeg(&pv.tex, &pv.tex_w, &pv.tex_h, s_buf.data(), got);
    return pv.tex ? (uintptr_t)pv.tex : 0;
}

// ── Preview API ───────────────────────────────────────────────────────────────

static void close_slot(PreviewState& pv) {
    if (pv.mjpeg_file) { fclose(pv.mjpeg_file); pv.mjpeg_file = nullptr; }
    if (pv.tex)        { glDeleteTextures(1, &pv.tex); pv.tex = 0; }
    pv.tex_w = pv.tex_h = 0;
    pv.last_frame_idx = -1;
    pv.is_open = pv.is_proxy = false;
    pv.proxy = {}; pv.info = {};
}

void video_open_still(int track_id, const std::string& jpeg_path) {
    if (track_id < 0 || track_id >= MAX_VIDEO_TRACKS) return;
    close_slot(g_pv[track_id]);

    FILE* f = fopen(jpeg_path.c_str(), "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) { fclose(f); return; }

    std::vector<uint8_t> buf((size_t)sz);
    fread(buf.data(), 1, (size_t)sz, f);
    fclose(f);

    upload_jpeg(&g_pv[track_id].tex, &g_pv[track_id].tex_w, &g_pv[track_id].tex_h,
                buf.data(), (size_t)sz);
    g_pv[track_id].is_open  = true;
    g_pv[track_id].is_proxy = false;
}

bool video_open_proxy(int track_id, const ProxyInfo& proxy) {
    if (track_id < 0 || track_id >= MAX_VIDEO_TRACKS) return false;
    close_slot(g_pv[track_id]);

    FILE* f = fopen(proxy.mjpeg_path.c_str(), "rb");
    if (!f) return false;

    PreviewState& pv = g_pv[track_id];
    pv.mjpeg_file      = f;
    pv.proxy           = proxy;
    pv.is_proxy        = true;
    pv.is_open         = true;
    pv.last_frame_idx  = -1;
    pv.info.width      = proxy.width;
    pv.info.height     = proxy.height;
    pv.info.fps        = proxy.fps;
    pv.info.duration   = proxy.fps > 0.0
                         ? (double)proxy.frame_count / proxy.fps : 0.0;

    // Show frame 0 immediately.
    uintptr_t tex = video_get_texture(track_id, 0.0);
    (void)tex;
    return true;
}

void video_close(int track_id) {
    if (track_id == -1) {
        for (int i = 0; i < MAX_VIDEO_TRACKS; ++i) close_slot(g_pv[i]);
        if (g_th.tex) { glDeleteTextures(1, &g_th.tex); g_th.tex = 0; }
        g_th.tex_w = g_th.tex_h = 0;
        g_th.last_frame_idx = -1;
    } else if (track_id >= 0 && track_id < MAX_VIDEO_TRACKS) {
        close_slot(g_pv[track_id]);
    }
}

bool      video_is_open(int track_id) {
    if (track_id < 0 || track_id >= MAX_VIDEO_TRACKS) return false;
    return g_pv[track_id].is_open;
}
VideoInfo video_info(int track_id) {
    if (track_id < 0 || track_id >= MAX_VIDEO_TRACKS) return {};
    return g_pv[track_id].info;
}

uintptr_t video_get_texture(int track_id, double playhead) {
    if (track_id < 0 || track_id >= MAX_VIDEO_TRACKS) return 0;
    PreviewState& pv = g_pv[track_id];
    if (!pv.is_open) return 0;

    if (!pv.is_proxy)
        return pv.tex ? (uintptr_t)pv.tex : 0;

    if (!pv.mjpeg_file || pv.proxy.offsets.empty()) return 0;

    double dur = pv.info.duration;
    if (playhead < 0.0) playhead = 0.0;
    if (dur > 0.0 && playhead > dur) playhead = dur;

    int64_t num = pv.proxy.fps_num;
    int64_t den = pv.proxy.fps_den;
    int frame_idx = (num > 0 && den > 0)
        ? (int)((int64_t)(playhead * (double)num) / den)
        : (int)(playhead * pv.proxy.fps);
    if (frame_idx >= (int)pv.proxy.offsets.size())
        frame_idx = (int)pv.proxy.offsets.size() - 1;
    if (frame_idx < 0) frame_idx = 0;

    if (frame_idx == pv.last_frame_idx && pv.tex)
        return (uintptr_t)pv.tex;

    pv.last_frame_idx = frame_idx;
    return decode_proxy_frame(pv, frame_idx);
}

uintptr_t video_get_thumbnail(double t, int* out_w, int* out_h) {
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    // Uses track 0's proxy for scrub bar hover preview
    PreviewState& pv = g_pv[0];
    if (!pv.is_open || !pv.is_proxy) return 0;
    if (!pv.mjpeg_file || pv.proxy.offsets.empty()) return 0;

    double dur = pv.info.duration;
    if (t < 0.0) t = 0.0;
    if (dur > 0.0 && t > dur) t = dur;

    int64_t num = pv.proxy.fps_num;
    int64_t den = pv.proxy.fps_den;
    int frame_idx = (num > 0 && den > 0)
        ? (int)((int64_t)(t * (double)num) / den)
        : (int)(t * pv.proxy.fps);
    if (frame_idx >= (int)pv.proxy.offsets.size())
        frame_idx = (int)pv.proxy.offsets.size() - 1;
    if (frame_idx < 0) frame_idx = 0;

    if (frame_idx == g_th.last_frame_idx && g_th.tex) {
        if (out_w) *out_w = g_th.tex_w;
        if (out_h) *out_h = g_th.tex_h;
        return (uintptr_t)g_th.tex;
    }
    g_th.last_frame_idx = frame_idx;

    uint64_t offset = pv.proxy.offsets[(size_t)frame_idx];
    bool is_last = ((size_t)frame_idx + 1 >= pv.proxy.offsets.size());
    size_t frame_sz = 0;
    if (!is_last) {
        frame_sz = (size_t)(pv.proxy.offsets[(size_t)frame_idx + 1] - offset);
    } else {
        fseeko(pv.mjpeg_file, (off_t)offset, SEEK_SET);
        long cur = ftell(pv.mjpeg_file);
        fseeko(pv.mjpeg_file, 0, SEEK_END);
        long end = ftell(pv.mjpeg_file);
        frame_sz = (end > cur) ? (size_t)(end - cur) : 0;
    }
    if (frame_sz == 0) return g_th.tex ? (uintptr_t)g_th.tex : 0;

    static std::vector<uint8_t> s_th_buf;
    s_th_buf.resize(frame_sz);
    fseeko(pv.mjpeg_file, (off_t)offset, SEEK_SET);
    size_t got = fread(s_th_buf.data(), 1, frame_sz, pv.mjpeg_file);
    if (got == 0) return g_th.tex ? (uintptr_t)g_th.tex : 0;

    upload_jpeg(&g_th.tex, &g_th.tex_w, &g_th.tex_h, s_th_buf.data(), got);

    if (out_w) *out_w = g_th.tex_w;
    if (out_h) *out_h = g_th.tex_h;
    return (uintptr_t)g_th.tex;
}

float video_probe_duration(const std::string& path) {
    AVFormatContext* fc = nullptr;
    if (avformat_open_input(&fc, path.c_str(), nullptr, nullptr) != 0) return 0.f;
    // find_stream_info is required for files where the container header doesn't
    // carry a reliable duration (e.g. some MP4/MKV variants).
    avformat_find_stream_info(fc, nullptr);
    float dur = (fc->duration != AV_NOPTS_VALUE)
        ? (float)fc->duration / (float)AV_TIME_BASE
        : 0.f;
    avformat_close_input(&fc);
    return dur;
}

// ── Export path — FFmpeg original file ───────────────────────────────────────

static struct ExportState {
    AVFormatContext* fmt_ctx    = nullptr;
    AVCodecContext*  codec_ctx  = nullptr;
    SwsContext*      sws        = nullptr;
    int              stream_idx = -1;
    VideoInfo        info       = {};
} g_ex;

bool video_open_export(const std::string& path) {
    video_close_export();

    if (avformat_open_input(&g_ex.fmt_ctx, path.c_str(), nullptr, nullptr) < 0)
        return false;
    if (avformat_find_stream_info(g_ex.fmt_ctx, nullptr) < 0) {
        avformat_close_input(&g_ex.fmt_ctx); return false;
    }
    for (unsigned i = 0; i < g_ex.fmt_ctx->nb_streams; ++i) {
        if (g_ex.fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            g_ex.stream_idx = (int)i; break;
        }
    }
    if (g_ex.stream_idx < 0) { avformat_close_input(&g_ex.fmt_ctx); return false; }

    AVStream* st = g_ex.fmt_ctx->streams[g_ex.stream_idx];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    g_ex.codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(g_ex.codec_ctx, st->codecpar);
    avcodec_open2(g_ex.codec_ctx, codec, nullptr);

    g_ex.info.width    = g_ex.codec_ctx->width;
    g_ex.info.height   = g_ex.codec_ctx->height;
    g_ex.info.duration = (double)g_ex.fmt_ctx->duration / AV_TIME_BASE;
    g_ex.info.fps      = av_q2d(st->avg_frame_rate);

    g_ex.sws = sws_getContext(
        g_ex.info.width, g_ex.info.height, g_ex.codec_ctx->pix_fmt,
        g_ex.info.width, g_ex.info.height, AV_PIX_FMT_RGBA,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    return true;
}

void video_close_export() {
    if (g_ex.sws)       { sws_freeContext(g_ex.sws);       g_ex.sws       = nullptr; }
    if (g_ex.codec_ctx) { avcodec_free_context(&g_ex.codec_ctx); }
    if (g_ex.fmt_ctx)   { avformat_close_input(&g_ex.fmt_ctx); }
    g_ex.stream_idx = -1;
    g_ex.info = {};
}

VideoFrame* video_decode_frame_at(double seconds) {
    if (!g_ex.fmt_ctx) return nullptr;

    int64_t ts = (int64_t)(seconds * AV_TIME_BASE);
    av_seek_frame(g_ex.fmt_ctx, -1, ts, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(g_ex.codec_ctx);

    AVPacket* pkt = av_packet_alloc();
    AVFrame*  frm = av_frame_alloc();
    VideoFrame* result = nullptr;

    while (av_read_frame(g_ex.fmt_ctx, pkt) >= 0 && !result) {
        if (pkt->stream_index != g_ex.stream_idx) { av_packet_unref(pkt); continue; }
        avcodec_send_packet(g_ex.codec_ctx, pkt);
        av_packet_unref(pkt);
        while (avcodec_receive_frame(g_ex.codec_ctx, frm) == 0 && !result) {
            result = new VideoFrame();
            result->width  = g_ex.info.width;
            result->height = g_ex.info.height;
            result->data   = (uint8_t*)av_malloc(
                (size_t)result->width * result->height * 4 + 64);
            AVStream* st = g_ex.fmt_ctx->streams[g_ex.stream_idx];
            result->pts = frm->pts * av_q2d(st->time_base);
            uint8_t* dst[1] = { result->data };
            int      lsz[1] = { result->width * 4 };
            sws_scale(g_ex.sws,
                (const uint8_t* const*)frm->data, frm->linesize,
                0, frm->height, dst, lsz);
            av_frame_unref(frm);
        }
    }

    av_packet_free(&pkt);
    av_frame_free(&frm);
    return result;
}
