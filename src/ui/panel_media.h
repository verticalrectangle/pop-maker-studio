#pragma once
// panel_media.h — media browsers + project/history panels

#include "studio_types.h"
#include "app.h"

void panel_history(AppState& state, float w);
void panel_project(AppState& state, float w);
void panel_media_browser(AppState& state, float w, bool is_video);
void panel_audio_browser(AppState& state, float w);
void panel_bin(AppState& state, float w);

// Media recents helpers (also used by timeline drag-drop and draw_preview OS drop)
enum class MediaKind { Video, Image, Audio };
struct RecentMedia { std::vector<std::string> videos, images, audio; };
RecentMedia& recent_media();
void recent_media_push(const std::string& path, MediaKind kind);
bool is_image_path(const std::string& p);

// Bin — project-scoped media library (lives on AppState.bin).
// bin_add is a no-op when the path is already present, so the same file
// dropped twice doesn't create duplicate entries.
MediaKind kind_for_path(const std::string& path);
bool      bin_contains   (const AppState& state, const std::string& path);
void      bin_add        (AppState& state, const std::string& path);
void      bin_remove     (AppState& state, const std::string& path);
int       bin_used_count (const AppState& state, const std::string& path);
// Backward compat for projects saved before the bin existed: derive the bin
// from unique video/audio clip paths already on the timeline. No-op when the
// bin is already populated.
void      bin_backfill_from_timeline(AppState& state);
