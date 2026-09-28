#pragma once
#include "app.h"
#include <string>

// Extract the embedded Inter Black font to /tmp for use by ffmpeg drawtext.
// Call once from app_init().
void render_init_fonts();
const std::string& render_font_path();

void render_cancel();
void render_snapshot_gl(AppState& state, float snap_t, bool open_folder = false); // GL path — matches preview exactly
bool render_export_srt(const AppState& state, const std::string& out_path);

// GL-based export: identical to live preview (same renderer, FBO → ffmpeg pipe).
// render_start_gl() initiates the export; render_tick_gl() must be called each
// frame from the main/GL thread until state.render.active becomes false.
void render_start_gl(AppState& state);
void render_tick_gl(AppState& state);
// Multi-format chain: called on the GL thread when one queued pass finishes.
// Starts the next pass (switching canvas + suffixed output) or restores the
// original canvas when the queue drains. Defined in engine_runtime.cpp.
void render_queue_advance(AppState& state);

// Platform preset helpers (preset table lives in render.cpp).
const char* render_platform_id(RenderPlatform p);
bool render_platform_from_id(const std::string& id, RenderPlatform& out);
// "" = canvas/fps valid for the active platform; else a human-readable warning.
std::string render_platform_check(const AppState& state);
// Render one track's active text overlay to a texture and composite it into the
// current scene (scene_add_layer) at that track's z-order, so text layers with
// video instead of always drawing on top. Call between scene_begin/scene_result
// inside the descending track loop; no-op when the track has no active text.
void scene_add_text_layer(const AppState& state, float t, int ti, int w, int h);

// Extract raw audio from a video file into a WAV, add as Audio track when done.
void extract_audio_start(AppState& state, const std::string& video_path);
