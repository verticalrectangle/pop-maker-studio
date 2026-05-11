#pragma once
// hf_api.h — HuggingFace model search + download via curl binary (no libraries)

#include <string>
#include <vector>
#include <atomic>

// ── Model record ──────────────────────────────────────────────────────────────

struct HFModel {
    std::string repo;       // e.g. "sail-rvc/Drake_RVC"
    std::string pth_file;   // first .pth found in siblings, e.g. "model.pth"
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

// Fire a background search. Results land in s.results; s.status → Done/Error.
void hf_search(const std::string& query, HFSearch& s);
// Cancel current search (bumps gen so the thread discards its result).
void hf_search_cancel(HFSearch& s);

// ── Download ──────────────────────────────────────────────────────────────────

struct HFDownload {
    enum class Status { Idle, Running, Done, Error };
    std::atomic<Status>    status{Status::Idle};
    std::atomic<uint64_t>  bytes_done{0};
    uint64_t               bytes_total = 0;  // set before thread; read-only thereafter
    std::string            out_path;
    std::string            tmp_path;
    std::string            error_msg;

    float progress() const {
        if (bytes_total == 0) return 0.f;
        return std::min(1.f, (float)bytes_done.load(std::memory_order_relaxed)
                             / (float)bytes_total);
    }
};

// Download <repo>/resolve/main/<filename> → out_path via curl.
// Call hf_download_poll() every frame while status == Running.
void hf_download_model(const std::string& repo, const std::string& filename,
                       const std::string& out_path, HFDownload& dl);

// Poll file-size progress (call each frame while status == Running).
void hf_download_poll(HFDownload& dl);

// ── Cache helpers ─────────────────────────────────────────────────────────────

std::string hf_rvc_cache_path(const std::string& filename);
bool        hf_rvc_installed(const std::string& filename);
