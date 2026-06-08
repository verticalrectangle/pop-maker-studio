#pragma once
#include <string>
#include <cstdint>
#include "proxy.h"
#include "app.h"
#include "bg_remove.h"

// Video preview — two separate paths:
//
// PREVIEW (proxy-based, stb_image, main thread only):
//   video_open_still()  — show a single JPEG while proxy is generating
//   video_open_proxy()  — open MJPEG + seek table; scrub is fseek + stb_image
//   video_get_texture() — decode current frame, upload to GL, return texture ID
//   video_close()
//
// EXPORT (FFmpeg, original file, frame-accurate):
//   video_open_export()         — open original for decode
//   video_decode_frame_at()     — decode exact frame at timestamp (blocks)
//   video_close_export()
//   VideoFrame                  — raw RGBA pixel data returned by decode_frame_at

struct VideoFrame {
    uint8_t* data   = nullptr;  // RGBA, width*height*4 bytes — caller must av_free()
    int      width  = 0;
    int      height = 0;
    double   pts    = 0.0;
};

struct VideoInfo {
    double duration  = 0.0;
    int    width     = 0;
    int    height    = 0;
    double fps       = 0.0;
    bool   has_audio = false;
};

// ── Preview path (multi-track) ────────────────────────────────────────────────

// MAX_VIDEO_TRACKS is defined in app.h (included above).
// Each video clip gets its own slot keyed by file path (see AppState::proxy_paths).
// track_id=-1 in video_close() closes all slots.

void      video_open_still(int track_id, const std::string& jpeg_path);
bool      video_open_proxy(int track_id, const ProxyInfo& proxy);
void      video_close(int track_id = -1);
bool      video_is_open(int track_id = 0);
VideoInfo video_info(int track_id = 0);
uintptr_t video_get_texture(int track_id, double playhead);

// Parallel pre-decode for multiple tracks. Each pair is (track_id, playhead).
// Runs JPEG decode + CPU FX in worker threads, then performs the GL uploads
// serially on the calling (main) thread. Subsequent video_get_texture() calls
// for the same (track, playhead) return the pre-decoded texture without
// redoing work. Safe to call with 0 or 1 entries (it falls through to direct
// decode in that case).
struct VideoPrefetchReq { int track_id; double playhead; };
void video_prefetch_frames(const VideoPrefetchReq* reqs, int n);

// Thumbnail for the scrub bar hover — always uses track 0's proxy.
uintptr_t video_get_thumbnail(double t, int* out_w, int* out_h);


// CPU pixel FX — set before video_get_texture(); applied during MJPEG decode.
// Call once per slot per render frame; all processing happens on the decoded pixels.
struct PixelFX {
    // Color grade (adjustment layer)
    float brightness  = 0.f;   // additive shift  (-1..+1)
    float contrast    = 1.f;   // scale around 0.5 (0..3)
    float saturation  = 1.f;   // mix luma↔color  (0..3)
    float hue_deg     = 0.f;   // Rodrigues rotation degrees
    float blur_sigma  = 0.f;   // Gaussian sigma (box-blur approximation)

    // Chroma key
    bool  chroma_key_on        = false;
    float chroma_key_r         = 0.f;
    float chroma_key_g         = 1.f;
    float chroma_key_b         = 0.f;
    float chroma_key_threshold = 0.30f;
    float chroma_key_softness  = 0.15f;

    // Glitch
    bool  glitch_on         = false;
    float glitch_chroma     = 0.f;
    float glitch_jitter     = 0.f;
    float glitch_corruption       = 0.f;
    float glitch_corruption_bleed = 0.f;

    // VHS
    bool  vhs_on       = false;
    float vhs_noise    = 0.f;
    float vhs_bleed    = 0.f;
    float vhs_tracking = 0.f;

    // Datamosh
    bool  datamosh_on        = false;
    float datamosh_intensity = 0.6f;
    float datamosh_spread    = 0.3f;

    // Remove Background
    bool        bg_remove_on       = false;
    std::string bg_remove_mask_dir;
    float       bg_remove_softness = 0.1f;
    bool        bg_remove_box_on   = false;
    float       bg_remove_box_l    = 0.f;
    float       bg_remove_box_r    = 1.f;
    float       bg_remove_box_t    = 0.f;
    float       bg_remove_box_b    = 1.f;

    float time         = 0.f;   // animation time (ImGui::GetTime())

    bool operator==(const PixelFX& o) const {
        return brightness == o.brightness && contrast   == o.contrast  &&
               saturation == o.saturation && hue_deg   == o.hue_deg   &&
               blur_sigma == o.blur_sigma &&
               chroma_key_on == o.chroma_key_on && chroma_key_r == o.chroma_key_r &&
               chroma_key_g == o.chroma_key_g && chroma_key_b == o.chroma_key_b &&
               chroma_key_threshold == o.chroma_key_threshold &&
               chroma_key_softness  == o.chroma_key_softness  &&
               glitch_on  == o.glitch_on  && glitch_chroma == o.glitch_chroma &&
               glitch_jitter == o.glitch_jitter && glitch_corruption == o.glitch_corruption &&
               glitch_corruption_bleed == o.glitch_corruption_bleed &&
               vhs_on     == o.vhs_on     && vhs_noise  == o.vhs_noise &&
               vhs_bleed  == o.vhs_bleed  && vhs_tracking == o.vhs_tracking &&
               datamosh_on == o.datamosh_on && datamosh_intensity == o.datamosh_intensity &&
               datamosh_spread == o.datamosh_spread &&
               bg_remove_on == o.bg_remove_on &&
               bg_remove_mask_dir == o.bg_remove_mask_dir &&
               bg_remove_softness == o.bg_remove_softness &&
               bg_remove_box_on == o.bg_remove_box_on &&
               bg_remove_box_l == o.bg_remove_box_l && bg_remove_box_r == o.bg_remove_box_r &&
               bg_remove_box_t == o.bg_remove_box_t && bg_remove_box_b == o.bg_remove_box_b &&
               time       == o.time;
    }
};
void video_set_pixel_fx(int track_id, const PixelFX& fx);

// FX browser preview thumbnail — 80×45 GL texture per FXType.
// Animated types (Glitch, VHS) regenerate each frame; others cached after first call.
uintptr_t video_fx_preview_texture(FXType ft, float t);

// Adjustment preset preview — 80×45 GL texture showing the grade applied to source.
// Keyed by unique_id; regenerated whenever called (caller caches by id if desired).
uintptr_t video_adj_preview_texture(int unique_id,
                                     float brightness, float contrast,
                                     float saturation, float hue,
                                     float blur, float vignette);

// Probe original video container for duration without full stream scan.
// Reads container header only — safe to call on the main thread, < 100 ms.
float video_probe_duration(const std::string& path);

struct MediaFileInfo {
    double duration    = 0.0;
    int    width       = 0;
    int    height      = 0;
    double fps         = 0.0;
    bool   has_video   = false;
    bool   has_audio   = false;
    std::string video_codec;
    std::string audio_codec;
    int    sample_rate = 0;
    int    channels    = 0;
    std::string error;
};
MediaFileInfo video_probe_file(const std::string& path);

// Extract a time segment from src into dst via stream-copy (no re-encode).
// Returns an empty string on success, or an error message on failure.
// dst should be a .webm or .mkv path; the container is inferred from the extension.
std::string video_extract_segment(const std::string& src,
                                  double start_sec, double end_sec,
                                  const std::string& dst);

// Browser thumbnail cache: load any JPEG/PNG file as a GL texture.
// Returns 0 if the file does not exist yet. Textures are cached by path
// for the session and never freed (one texture per source file, ~few KB each).
// out_w / out_h receive the image dimensions (may be nullptr).
uintptr_t video_load_thumb(const std::string& jpeg_or_png_path,
                           int* out_w = nullptr, int* out_h = nullptr);

// ── Export path ───────────────────────────────────────────────────────────────
//
// Each export slot (0 .. MAX_VIDEO_TRACKS*2-1) has its own independent FFmpeg
// decoder context.  This prevents transition frames from disturbing each other's
// sequential-decode state (no seek thrash when two clips overlap).
//
// slot defaults to 0 so callers outside render.cpp (snapshot path, tests) work
// without change.

bool  video_open_export   (int slot, const std::string& path);
void  video_close_export  (int slot = 0);
void  video_close_export_all();  // close every slot (call at end of render)

// Frame-accurate single-frame decode.  Caller must call video_free_frame().
VideoFrame* video_decode_frame_at(int slot, double seconds);
void        video_free_frame(VideoFrame* f);  // av_free(data) + delete

// Returns dimensions of the currently open export file (0,0 if none open).
int video_export_width (int slot = 0);
int video_export_height(int slot = 0);

// Apply AI bg-remove alpha mask to a decoded export frame in-place.
// Reads the per-frame grayscale JPEG from cl.bg_remove_mask_dir / bg_masks.mjpeg.
// The mask is bilinearly scaled to match vf dimensions if they differ.
// No-op if bg_remove_on is false, mask_dir is empty, or the frame is not found.
void video_apply_bg_remove_export(VideoFrame* vf, const Clip& cl, int mask_frame_idx);

// Apply CPU JPEG datamosh corruption to an RGBA VideoFrame in-place.
// Encodes to JPEG, corrupts scan bytes, decodes back.  Modifies vf->data.
// Skips if intensity <= 0 or encode fails.
void video_apply_datamosh(VideoFrame* vf, float intensity, float time_sec);
