#pragma once
// pexels_api.h — Pexels stock photo/video search + download, curl binary only
// (same no-library approach as hf_api.h). Downloads are USER CONTENT and live
// under the managed share dir (~/.local/share/pop-maker-studio/pexels), NOT
// the regeneratable media cache — cache_clear()/cache_prune() must never
// delete them. Thumbnails ARE regeneratable → media cache.

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

enum class PexelsKind { Video, Photo };

// ── Search ────────────────────────────────────────────────────────────────────

struct PexelsFilters {
    enum Orientation { OriAny = 0, OriLandscape, OriPortrait, OriSquare };
    int orientation = OriAny;      // PexelsFilters::Orientation
};

struct PexelsItem {
    long long   id = 0;           // Pexels media id
    std::string photographer;    // photos: photographer; videos: user.name
    std::string alt;             // alt text / description
    int         duration = 0;    // seconds (videos only; 0 for photos)
    int         width = 0, height = 0;  // of the chosen download variant
    std::string thumb_url;       // small JPEG for grid cards
    std::string download_url;    // best <=1080p h264 .mp4 (videos) / large2x .jpg (photos)
};

struct PexelsSearch {
    enum class Status { Idle, Running, Done, Error };
    std::atomic<Status>   status{Status::Idle};
    std::atomic<uint32_t> gen{0};      // bump to cancel in-flight request
    std::vector<PexelsItem> results;   // valid only when status == Done
    int  page = 0;                     // page these results came from
    bool has_more = false;
    std::string error;
};

// Search Pexels (photos: GET /v1/search, videos: GET /v1/videos/search).
// per_page fixed at 15. `page` >= 1; callers accumulate results across pages.
// Missing/invalid key → status Error with error "No Pexels API key..." /
// "Invalid Pexels API key" (HTTP 401).
void pexels_search(PexelsKind kind, const std::string& query,
                   const PexelsFilters& f, int page, PexelsSearch& s);
void pexels_search_cancel(PexelsSearch& s);

// ── Download ──────────────────────────────────────────────────────────────────

struct PexelsDownload {
    enum class Status { Idle, Running, Done, Error };
    std::atomic<Status>   status{Status::Idle};
    std::atomic<uint64_t> bytes_done{0};
    uint64_t              bytes_total = 0;
    std::string           out_path;   // final path once Done
    std::string           error_msg;

    float progress() const {
        if (bytes_total == 0) return 0.f;
        uint64_t d = bytes_done.load(std::memory_order_relaxed);
        return d >= bytes_total ? 1.f : (float)d / (float)bytes_total;
    }
};

// Where this item downloads to (file exists ⇒ already downloaded):
//   <managed>/pexels/videos/pexels-<id>-<slug>.mp4
//   <managed>/pexels/photos/pexels-<id>-<slug>.jpg
// slug = lowercased photographer, non-alphanumerics collapsed to '-'.
std::string pexels_download_path(const PexelsItem& item, PexelsKind kind);
bool pexels_downloaded(const PexelsItem& item, PexelsKind kind);

// Fetch download_url → pexels_download_path() (tmp .part file + rename on
// success). bytes_total from Content-Length. No auth header needed — media
// URLs are public. Call pexels_download_poll() every frame while Running.
void pexels_download(const PexelsItem& item, PexelsKind kind, PexelsDownload& dl);
void pexels_download_poll(PexelsDownload& dl);

// ── Thumbnails ────────────────────────────────────────────────────────────────

// Queue id+url for a background fetch (deduped; ~4 curls in flight) into
// pexels_thumb_path(id). Fire-and-forget; callers re-check the file each
// frame and load it once it exists.
void pexels_thumb_fetch(long long id, const std::string& url);
// <media_cache_dir>/pexels/thumbs/<id>.jpg — regeneratable, LRU-pruned.
std::string pexels_thumb_path(long long id);

// ── API key ───────────────────────────────────────────────────────────────────
// Env PEXELS_API_KEY wins (dev/test), else secret-tool keyring with attributes
// service=pexels key=api, label 'Pop Maker Studio Pexels'. Same scheme as the
// agent key (agent_key_* in agent_harness.cpp). The key never touches disk
// files other than the keyring.

std::string pexels_key();
bool pexels_key_available();                   // secret-tool binary exists
bool pexels_key_present();                     // a usable key exists
bool pexels_key_store(const std::string& key); // returns false if no keyring
bool pexels_key_clear();
