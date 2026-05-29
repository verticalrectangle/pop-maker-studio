#include "vision_download.h"
#include "paths.h"

#include <filesystem>
#include <atomic>
#include <mutex>
#include <thread>
#include <string>
#include <vector>
#include <cstdio>
namespace fs = std::filesystem;

// ── File manifest ─────────────────────────────────────────────────────────────

// Each model file: HF subpath, destination name, size (bytes for progress weight)
struct ModelFile {
    const char* hf_subpath;   // relative path inside the HF snapshot
    const char* dest_name;    // filename in app_models_dir()
    uint64_t    size_bytes;   // approximate; used to weight overall progress
};

static const ModelFile FILES[] = {
    { "onnx/vision_encoder_q4.onnx",      "moondream-encoder.onnx",    293'000'000ULL },
    { "onnx/decoder_model_merged_q4.onnx","moondream-decoder.onnx",    864'000'000ULL },
    { "tokenizer.json",                    "moondream-tokenizer.json",    2'000'000ULL },
};
static const int N_FILES = (int)(sizeof(FILES) / sizeof(FILES[0]));
static const float TOTAL_BYTES_F = (float)(293'000'000ULL + 864'000'000ULL + 2'000'000ULL);

static const char* HF_BASE =
    "https://huggingface.co/Xenova/moondream2/resolve/main/";

// ── Static state ──────────────────────────────────────────────────────────────

static struct {
    std::atomic<bool>  running{false};
    std::atomic<bool>  done{false};
    std::atomic<bool>  cancel{false};
    std::atomic<float> progress{0.f};
    std::string        message;
    std::string        error;
    std::mutex         mu;
} s;

// ── Helpers ───────────────────────────────────────────────────────────────────

static void set_message(const std::string& msg) {
    std::lock_guard<std::mutex> lk(s.mu);
    s.message = msg;
}

static void set_error(const std::string& err) {
    std::lock_guard<std::mutex> lk(s.mu);
    s.error = err;
}

// Scan ~/.cache/huggingface/hub/models--Xenova--moondream2/snapshots/*/ for the
// given subpath. Returns the first path found, or empty string.
static std::string find_in_hf_cache(const char* subpath) {
    const char* home = getenv("HOME");
    if (!home) return {};
    fs::path base = fs::path(home) / ".cache" / "huggingface" / "hub"
                    / "models--Xenova--moondream2" / "snapshots";
    std::error_code ec;
    if (!fs::is_directory(base, ec)) return {};
    for (auto& snap : fs::directory_iterator(base, ec)) {
        fs::path candidate = snap.path() / subpath;
        if (fs::exists(candidate, ec))
            return candidate.string();
    }
    return {};
}

// Download a single file via curl subprocess into a .dl temp file, then rename.
// Updates overall progress incrementally.
// Returns false on failure or cancellation.
static bool download_file(const ModelFile& mf,
                           uint64_t bytes_before,   // bytes from earlier files
                           const std::string& dest) {
    std::string tmp = dest + ".dl";
    std::string url = std::string(HF_BASE) + mf.hf_subpath;

    // Build curl command (same pattern as hf_api.cpp)
    std::string cmd = "curl -L --max-time 3600 --fail -s -o \""
                      + tmp + "\" \"" + url + "\" 2>&1";

    // Remove stale tmp if present
    fs::remove(tmp);

    FILE* p = popen(cmd.c_str(), "r");
    if (!p) { set_error("curl not found"); return false; }

    // Poll tmp file size for progress while curl runs
    while (true) {
        if (s.cancel.load(std::memory_order_relaxed)) {
            pclose(p);
            fs::remove(tmp);
            return false;
        }
        // Non-blocking check: feof on popen pipe
        // (curl writes nothing to stdout with -s, pipe stays open until done)
        std::error_code ec;
        uint64_t got = fs::file_size(tmp, ec);
        if (!ec) {
            uint64_t total_done = bytes_before + got;
            s.progress.store(
                std::min(1.f, (float)total_done / TOTAL_BYTES_F),
                std::memory_order_relaxed);
        }
        // Check if pipe is done
        int c = fgetc(p);
        if (c == EOF) break;
        // Discard any unexpected output (curl shouldn't produce any with -s)
        (void)c;
    }

    int ret = pclose(p);
    if (ret != 0 || s.cancel.load()) {
        fs::remove(tmp);
        if (!s.cancel.load())
            set_error("curl failed downloading " + std::string(mf.dest_name));
        return false;
    }

    std::error_code ec;
    fs::rename(tmp, dest, ec);
    if (ec) {
        // rename across filesystems: try copy + remove
        fs::copy_file(tmp, dest, fs::copy_options::overwrite_existing, ec);
        fs::remove(tmp);
        if (ec) { set_error("failed to install " + std::string(mf.dest_name)); return false; }
    }
    return true;
}

// ── Download thread ───────────────────────────────────────────────────────────

static void run_download() {
    std::string models_dir = app_models_dir();

    // Phase 1: check HF cache for all files
    std::vector<std::string> cache_paths(N_FILES);
    bool all_cached = true;
    for (int i = 0; i < N_FILES; i++) {
        cache_paths[i] = find_in_hf_cache(FILES[i].hf_subpath);
        if (cache_paths[i].empty()) { all_cached = false; break; }
    }

    if (all_cached) {
        // Copy from HF cache — fast path
        uint64_t bytes_done = 0;
        for (int i = 0; i < N_FILES; i++) {
            if (s.cancel.load()) { s.running.store(false); return; }
            set_message(std::string("Copying ") + FILES[i].dest_name + " from cache…");
            std::string dest = models_dir + "/" + FILES[i].dest_name;
            std::error_code ec;
            fs::copy_file(cache_paths[i], dest, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                set_error("failed to copy " + std::string(FILES[i].dest_name)
                          + ": " + ec.message());
                s.done.store(true); s.running.store(false); return;
            }
            bytes_done += FILES[i].size_bytes;
            s.progress.store(std::min(1.f, (float)bytes_done / TOTAL_BYTES_F),
                             std::memory_order_relaxed);
        }
    } else {
        // Download each file from HuggingFace
        uint64_t bytes_before = 0;
        for (int i = 0; i < N_FILES; i++) {
            if (s.cancel.load()) { s.running.store(false); return; }

            std::string dest = models_dir + "/" + FILES[i].dest_name;
            if (fs::exists(dest)) {
                // Already present from a previous partial download
                bytes_before += FILES[i].size_bytes;
                s.progress.store(std::min(1.f, (float)bytes_before / TOTAL_BYTES_F),
                                 std::memory_order_relaxed);
                continue;
            }

            char size_buf[32];
            uint64_t mb = FILES[i].size_bytes / 1'000'000ULL;
            snprintf(size_buf, sizeof(size_buf), "%llu MB", (unsigned long long)mb);
            set_message(std::string("Downloading ") + FILES[i].dest_name
                        + " (" + size_buf + ")…");

            if (!download_file(FILES[i], bytes_before, dest)) {
                // error already set inside download_file (or cancel was requested)
                s.done.store(true); s.running.store(false); return;
            }
            bytes_before += FILES[i].size_bytes;
        }
    }

    s.progress.store(1.f, std::memory_order_relaxed);
    set_message("Vision model ready.");
    s.done.store(true);
    s.running.store(false);
}

// ── Public API ────────────────────────────────────────────────────────────────

bool vision_models_ready() {
    std::string dir = app_models_dir();
    for (int i = 0; i < N_FILES; i++) {
        if (!fs::exists(dir + "/" + FILES[i].dest_name))
            return false;
    }
    return true;
}

bool vision_download_start() {
    if (s.running.load()) return false;
    if (vision_models_ready()) { s.done.store(true); return false; }
    s.running.store(true);
    s.done.store(false);
    s.cancel.store(false);
    s.progress.store(0.f);
    { std::lock_guard<std::mutex> lk(s.mu); s.message.clear(); s.error.clear(); }
    std::thread(run_download).detach();
    return true;
}

bool  vision_download_running()  { return s.running.load(); }
bool  vision_download_done()     { return s.done.load() && !s.running.load(); }
float vision_download_progress() { return s.progress.load(); }

std::string vision_download_message() {
    std::lock_guard<std::mutex> lk(s.mu); return s.message;
}
std::string vision_download_error() {
    std::lock_guard<std::mutex> lk(s.mu); return s.error;
}

void vision_download_cancel() {
    s.cancel.store(true);
    s.running.store(false);
    // Clean up any in-progress .dl temp files
    std::string dir = app_models_dir();
    for (int i = 0; i < N_FILES; i++) {
        std::string tmp = dir + "/" + FILES[i].dest_name + ".dl";
        fs::remove(tmp);
    }
}
