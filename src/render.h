#pragma once
#include "app.h"
#include <string>

// Extract the embedded Inter Black font to /tmp for use by ffmpeg drawtext.
// Call once from app_init().
void render_init_fonts();
const std::string& render_font_path();

void render_start(AppState& state);
void render_cancel();
void render_snapshot_start(AppState& state, float snap_t);  // legacy ffmpeg path
void render_snapshot_gl(AppState& state, float snap_t);     // GL path — matches preview exactly
bool render_export_srt(const AppState& state, const std::string& out_path);

// GL-based export: identical to live preview (same renderer, FBO → ffmpeg pipe).
// render_start_gl() initiates the export; render_tick_gl() must be called each
// frame from the main/GL thread until state.render.active becomes false.
void render_start_gl(AppState& state);
void render_tick_gl(AppState& state);

// Extract raw audio from a video file into a WAV, add as Audio track when done.
void extract_audio_start(AppState& state, const std::string& video_path);
