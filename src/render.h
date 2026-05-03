#pragma once
#include "app.h"
#include <string>

// Extract the embedded Inter Black font to /tmp for use by ffmpeg drawtext.
// Call once from app_init().
void render_init_fonts();
const std::string& render_font_path();

void render_start(AppState& state);
void render_cancel();
bool render_export_srt(const AppState& state, const std::string& out_path);

// Extract raw audio from a video file into a WAV, add as Audio track when done.
void extract_audio_start(AppState& state, const std::string& video_path);
