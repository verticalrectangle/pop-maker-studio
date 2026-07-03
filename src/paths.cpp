#include "paths.h"
#include "globals.h"

// Managed-storage root (takes, proxies, recovery). Defined engine-side; the
// app main() fills it during startup.
std::string g_managed_dir;
std::atomic<bool> g_shutdown{false};
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <vector>
#include <algorithm>
#include <sys/stat.h>
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

std::string projects_dir() {
    const char* home = getenv("HOME");
    std::string dir = (home ? std::string(home) : std::string("."))
                    + "/Videos/Pop Maker Studio Projects";
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
// ── Cache maintenance ─────────────────────────────────────────────────────────
static const unsigned long long DEFAULT_CACHE_CAP = 8ull * 1024 * 1024 * 1024; // 8 GiB

unsigned long long cache_size_bytes() {
    unsigned long long total = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(media_cache_dir(), ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec)) total += (unsigned long long)it->file_size(ec);
    }
    return total;
}

unsigned long long cache_clear() {
    unsigned long long freed = 0;
    std::error_code ec;
    for (auto it = fs::directory_iterator(media_cache_dir(), ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        unsigned long long sz = it->is_regular_file(e2) ? (unsigned long long)it->file_size(e2) : 0;
        if (it->is_directory(e2)) {
            for (auto jt = fs::recursive_directory_iterator(it->path(), e2);
                 !e2 && jt != fs::recursive_directory_iterator(); jt.increment(e2))
                if (jt->is_regular_file(e2)) sz += (unsigned long long)jt->file_size(e2);
        }
        if (fs::remove_all(it->path(), e2) != static_cast<std::uintmax_t>(-1) && !e2)
            freed += sz;
    }
    return freed;
}

unsigned long long cache_prune(unsigned long long cap_bytes) {
    if (cap_bytes == 0) cap_bytes = DEFAULT_CACHE_CAP;

    struct Entry { std::string path; unsigned long long size; long long atime; };
    std::vector<Entry> files;
    unsigned long long total = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(media_cache_dir(), ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2)) continue;
        unsigned long long sz = (unsigned long long)it->file_size(e2);
        struct stat st{};
        long long at = (stat(it->path().c_str(), &st) == 0) ? (long long)st.st_atime : 0;
        files.push_back({it->path().string(), sz, at});
        total += sz;
    }
    if (total <= cap_bytes) return 0;

    // Oldest access first → delete until back under the cap.
    std::sort(files.begin(), files.end(),
              [](const Entry& a, const Entry& b){ return a.atime < b.atime; });
    unsigned long long freed = 0;
    for (auto& f : files) {
        if (total - freed <= cap_bytes) break;
        std::error_code e2;
        if (fs::remove(f.path, e2) && !e2) freed += f.size;
    }
    return freed;
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
