#pragma once
#include <atomic>
#include <string>
std::string app_models_dir();    // <binary_dir>/models
std::string wav2vec2_ctc_path(); // <binary_dir>/models/wav2vec2_ctc.onnx

// ── Centralized media cache ───────────────────────────────────────────────────
// Derived, regeneratable artifacts (proxies, conforms, stills, bg masks) live
// in one cache dir instead of being scattered next to every source file.
// XDG_CACHE_HOME/pop-maker-studio/media (created on first use).
std::string media_cache_dir();
// A cache file path for `source` + `suffix`, keyed by a hash of the source path
// (stable, collision-resistant; the source's stem is kept as a readable prefix).
// e.g. cache_path("/x/clip.mp4", ".proxy.mjpeg") -> <cache>/clip.<hash>.proxy.mjpeg
std::string cache_path(const std::string& source, const std::string& suffix);

// Set true by app_shutdown; background threads check this to exit early.
extern std::atomic<bool> g_shutdown;
