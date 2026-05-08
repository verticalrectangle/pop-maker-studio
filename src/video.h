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

// ── Export path ───────────────────────────────────────────────────────────────

bool  video_open_export(const std::string& path);
void  video_close_export();

// Frame-accurate single-frame decode.  Caller must call video_free_frame().
VideoFrame* video_decode_frame_at(double seconds);
void        video_free_frame(VideoFrame* f);  // av_free(data) + delete

// Returns dimensions of the currently open export file (0,0 if none open).
int video_export_width();
int video_export_height();
