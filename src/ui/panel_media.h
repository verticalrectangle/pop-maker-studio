#pragma once
// panel_media.h — media browsers + project/history panels

#include "studio_types.h"
#include "app.h"

void panel_history(AppState& state, float w);
void panel_project(AppState& state, float w);
void panel_media_browser(AppState& state, float w, bool is_video);
void panel_audio_browser(AppState& state, float w);

// Media recents helpers (also used by timeline drag-drop and draw_preview OS drop)
enum class MediaKind { Video, Image, Audio };
struct RecentMedia { std::vector<std::string> videos, images, audio; };
RecentMedia& recent_media();
void recent_media_push(const std::string& path, MediaKind kind);
bool is_image_path(const std::string& p);
