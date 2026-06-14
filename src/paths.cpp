#include "paths.h"
std::atomic<bool> g_shutdown{false};
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
namespace fs = std::filesystem;

std::string media_cache_dir() {
    const char* xdg = getenv("XDG_CACHE_HOME");
    const char* home = getenv("HOME");
    std::string base = (xdg && *xdg) ? std::string(xdg)
                     : home          ? std::string(home) + "/.cache"
                                     : std::string("/tmp");
    std::string dir = base + "/pop-maker-studio/media";
    std::error_code ec; fs::create_directories(dir, ec);
    return dir;
}

std::string cache_path(const std::string& source, const std::string& suffix) {
    // FNV-1a hash of the source path → stable, filename-safe, collision-resistant.
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : source) { h ^= c; h *= 1099511628211ull; }
    char hx[20]; snprintf(hx, sizeof(hx), "%016llx", (unsigned long long)h);
    std::string stem = fs::path(source).stem().string();
    return media_cache_dir() + "/" + stem + "." + hx + suffix;
}
std::string app_models_dir() {
    char buf[4096] = {};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf)-1);
    if (n <= 0) return "models";
    return (fs::path(buf).parent_path() / "models").string();
}

std::string wav2vec2_ctc_path() {
    return (fs::path(app_models_dir()) / "wav2vec2_ctc.onnx").string();
}
