// media_bin.cpp — project media-bin management + path sniffing, hoisted from
// ui/panel_media.cpp during the iOS engine extraction (Phase 0). The bin is
// project state (serialized in .pms); the panel that DISPLAYS it stays
// app-side.
#include "engine_seams.h"
#include "app.h"
#include "history.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
namespace fs = std::filesystem;
#include <fstream>
#include "json.hpp"
using nlohmann::json;

// ── Media browser helpers ─────────────────────────────────────────────────────

static std::string media_recents_path() {
    const char* h = getenv("HOME");
    return h ? std::string(h) + "/.config/pop-maker-studio/recent_media.json"
             : "/tmp/pop_maker_recent_media.json";
}

// RecentMedia struct defined in panel_media.h

RecentMedia& recent_media() {
    static RecentMedia s;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        try {
            std::ifstream f(media_recents_path());
            if (f) {
                auto j = nlohmann::json::parse(f, nullptr, false);
                if (!j.is_discarded()) {
                    if (j.contains("videos")) s.videos = j["videos"].get<std::vector<std::string>>();
                    if (j.contains("images")) s.images = j["images"].get<std::vector<std::string>>();
                    if (j.contains("audio"))  s.audio  = j["audio"].get<std::vector<std::string>>();
                }
            }
        } catch (...) {}
    }
    return s;
}


bool is_image_path(const std::string& p) {
    fs::path fp(p);
    std::string ext = fp.extension().string();
    for (auto& c : ext) c = (char)tolower((unsigned char)c);
    return ext==".jpg"||ext==".jpeg"||ext==".png"||ext==".bmp"||ext==".webp"||ext==".tiff"
        || ext==".heic"||ext==".heif"||ext==".gif"||ext==".svg";
}

void bin_add(AppState& state, const std::string& path) {
    if (path.empty() || bin_contains(state, path)) return;
    state.bin.push_back(path);
    // Mirror into the cross-project Recent list so it shows up next time too.
    recent_media_push(path, kind_for_path(path));
}

void bin_remove(AppState& state, const std::string& path) {
    state.bin.erase(std::remove(state.bin.begin(), state.bin.end(), path),
                    state.bin.end());
}

void bin_backfill_from_timeline(AppState& state) {
    if (!state.bin.empty()) return;
    for (auto& tr : state.tracks) {
        for (auto& cl : tr.clips) {
            if ((cl.clip_type == ClipType::Video || cl.clip_type == ClipType::Audio)
                && !cl.text.empty())
                bin_add(state, cl.text);
        }
    }
}

// MediaKind enum in panel_media.h { Video, Image, Audio };

void recent_media_push(const std::string& path, MediaKind kind) {
    auto& r   = recent_media();
    auto& vec = kind == MediaKind::Video ? r.videos
              : kind == MediaKind::Audio ? r.audio
                                        : r.images;
    vec.erase(std::remove(vec.begin(), vec.end(), path), vec.end());
    vec.insert(vec.begin(), path);
    if (vec.size() > 24) vec.resize(24);
    try {
        nlohmann::json j;
        j["videos"] = r.videos;
        j["images"] = r.images;
        j["audio"]  = r.audio;
        fs::create_directories(fs::path(media_recents_path()).parent_path());
        std::ofstream(media_recents_path()) << j.dump(2);
    } catch (...) {}
}

// ── Bin helpers ──────────────────────────────────────────────────────────────

MediaKind kind_for_path(const std::string& path) {
    if (is_image_path(path)) return MediaKind::Image;
    fs::path fp(path);
    std::string ext = fp.extension().string();
    for (auto& c : ext) c = (char)tolower((unsigned char)c);
    // Anything with a video container extension counts as Video; everything
    // else audio-shaped counts as Audio. Matches what add_clip_to_track does
    // when classifying drops, so the bin's kind tag agrees with how the clip
    // would actually be placed.
    if (ext==".mp4"||ext==".mov"||ext==".mkv"||ext==".avi"||ext==".webm") return MediaKind::Video;
    return MediaKind::Audio;
}

bool bin_contains(const AppState& state, const std::string& path) {
    for (auto& p : state.bin) if (p == path) return true;
    return false;
}

// An animated image is image-kind (silent, no audio track — see the audio_path
// guard in add_clip) but plays back like a video: it gets a full MJPEG proxy
// (proxy.cpp's is_image_ext deliberately excludes .gif for exactly this) and
// must NOT be pinned to a single Still in the preview, or it freezes on frame 0.
bool is_animated_image(const std::string& p) {
    fs::path fp(p);
    std::string ext = fp.extension().string();
    for (auto& c : ext) c = (char)tolower((unsigned char)c);
    return ext==".gif";
}
