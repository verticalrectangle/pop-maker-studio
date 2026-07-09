#pragma once
#include <atomic>
#include <string>

// ── Asset root override ───────────────────────────────────────────────────────
// On iOS the bundle path arrives through pms_create(asset_root); on desktop no
// override is set and paths stay binary-relative (readlink /proc/self/exe).
void pms_set_asset_root(std::string root);
// The effective asset root: the override when set, else the binary's directory.
std::string app_asset_root();
// Models directory. With an asset-root override: bundled models under
// <asset_root>/models first, else downloaded packs under <state_root>/models.
// Without an override (desktop): <binary_dir>/models, unchanged.
std::string app_models_dir();
std::string wav2vec2_ctc_path(); // app_models_dir()/wav2vec2_ctc.onnx

// ── Centralized media cache ───────────────────────────────────────────────────
// Derived, regeneratable artifacts (proxies, conforms, stills, bg masks) live
// in one cache dir instead of being scattered next to every source file.
// XDG_CACHE_HOME/pop-maker-studio/media (created on first use).
std::string media_cache_dir();
// User-facing project location: ~/Videos/Pop Maker Studio Projects (created).
std::string projects_dir();
// A cache file path for `source` + `suffix`, keyed by a hash of the source path
// (stable, collision-resistant; the source's stem is kept as a readable prefix).
// e.g. cache_path("/x/clip.mp4", ".proxy.mjpeg") -> <cache>/clip.<hash>.proxy.mjpeg
std::string cache_path(const std::string& source, const std::string& suffix);

// ── Cache maintenance ─────────────────────────────────────────────────────────
// Total bytes currently held in the media cache (recursive).
unsigned long long cache_size_bytes();
// Delete every file in the media cache. Returns bytes freed. Safe to call while a
// project is open — proxies/conforms/stills regenerate lazily on next use.
unsigned long long cache_clear();
// LRU-prune the cache down to `cap_bytes` by deleting least-recently-accessed
// files first. No-op when already under the cap. Returns bytes freed. Intended to
// run at startup (nothing in use yet). Pass 0 to use the built-in default cap.
unsigned long long cache_prune(unsigned long long cap_bytes = 0);

// Set true by app_shutdown; background threads check this to exit early.
extern std::atomic<bool> g_shutdown;
