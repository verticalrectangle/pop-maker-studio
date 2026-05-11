#pragma once
// hf_api.h — HuggingFace model search + download via curl binary (no libraries)

#include <string>
#include <vector>
#include <atomic>

// ── Model record ──────────────────────────────────────────────────────────────

struct HFModel {
    std::string repo;        // e.g. "binant/Drake_RVC"
    std::string model_file;  // .pth preferred; .zip if no bare .pth in repo
    int         downloads = 0;
};

// ── Search ────────────────────────────────────────────────────────────────────

struct HFSearch {
    enum class Status { Idle, Running, Done, Error };
    std::atomic<Status>   status{Status::Idle};
    std::atomic<uint32_t> gen{0};      // bump to cancel in-flight request
    std::vector<HFModel>  results;     // valid only when status == Done
    std::string           error;
};

void hf_search(const std::string& query, HFSearch& s);
void hf_search_cancel(HFSearch& s);

// ── Download ──────────────────────────────────────────────────────────────────

struct HFDownload {
    enum class Status { Idle, Running, Done, Error };
    std::atomic<Status>    status{Status::Idle};
    std::atomic<uint64_t>  bytes_done{0};
    uint64_t               bytes_total = 0;
    std::string            out_path;   // final .pth path after download/extract
    std::string            tmp_path;   // temp file being written (for progress poll)
    std::string            error_msg;

    float progress() const {
        if (bytes_total == 0) return 0.f;
        return std::min(1.f, (float)bytes_done.load(std::memory_order_relaxed)
                             / (float)bytes_total);
    }
};

// Download repo/resolve/main/model_file → final .pth at out_path.
// If model_file is a .zip, extracts the .pth after download.
// Call hf_download_poll() every frame while status == Running.
void hf_download_model(const std::string& repo, const std::string& model_file,
                       const std::string& out_path, HFDownload& dl);

void hf_download_poll(HFDownload& dl);

// ── Cache helpers ─────────────────────────────────────────────────────────────

// Returns the .pth path that will exist after download completes.
// repo is used to namespace the cache (avoids collisions when multiple
// repos share the same filename, e.g. model.pth).
std::string hf_rvc_model_path(const std::string& repo,
                               const std::string& model_file);

bool hf_rvc_installed(const std::string& repo, const std::string& model_file);
