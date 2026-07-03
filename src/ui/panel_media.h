#pragma once
#include "../engine_seams.h"  // MediaKind, bin_*, recent_media_push (hoisted)
// panel_media.h — media browsers + project/history panels

#include "studio_types.h"
#include "app.h"

void panel_history(AppState& state, float w);
void panel_project(AppState& state, float w);
void panel_media_browser(AppState& state, float w, bool is_video);
void panel_audio_browser(AppState& state, float w);
void panel_bin(AppState& state, float w);

// Media recents helpers (also used by timeline drag-drop and draw_preview OS drop)
bool is_image_path(const std::string& p);

// Bin — project-scoped media library (lives on AppState.bin).
// bin_add is a no-op when the path is already present, so the same file
// dropped twice doesn't create duplicate entries.
const char* media_kind_label(MediaKind k);             // "video" | "image" | "audio"
bool      is_media_path  (const std::string& path);    // true for known video/audio/image extensions
bool      path_is_dir    (const std::string& path);
// Shallow (non-recursive) list of media files directly inside `dir`, sorted, capped.
std::vector<std::string> dir_media_files(const std::string& dir, int cap = 200);
void      bin_add        (AppState& state, const std::string& path);
void      bin_remove     (AppState& state, const std::string& path);
int       bin_used_count (const AppState& state, const std::string& path);
// Backward compat for projects saved before the bin existed: derive the bin
// from unique video/audio clip paths already on the timeline. No-op when the
// bin is already populated.
void      bin_backfill_from_timeline(AppState& state);
