#pragma once
#include "app.h"
#include <string>

// Render the lyric video to MP4 using libavcodec.
// Runs in a background thread; writes progress to RenderStatus.
// Stub for tonight — SRT export is functional, MP4 render is scaffolded.
void render_start(AppState& state);
void render_cancel();

// Export word-level SRT — always available once transcription is done
bool render_export_srt(const std::vector<LyricLine>& lines, const std::string& out_path);
