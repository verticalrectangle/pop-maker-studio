// pexels_api.cpp — Pexels stock photo/video search + download, curl binary only
// (same no-library approach as hf_api.cpp). Downloads are USER CONTENT and live
// under ~/.local/share/pop-maker-studio/pexels (NOT the regeneratable media
// cache — cache_clear()/cache_prune() must never delete them). Thumbnails ARE
// regeneratable → media cache.

#include "pexels_api.h"
#include "platform.h"
#include "paths.h"
#include "json.hpp"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <condition_variable>
#include <signal.h>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_set>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/stat.h>

namespace fs = std::filesystem;

using json = nlohmann::json;

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::string url_encode(const std::string& s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out += (char)c;
        else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

static int run_cmd_status(const std::string& cmd) {
    int rc = pms_system(cmd.c_str());
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

// ── API key (secret-tool, env override) ───────────────────────────────────────

bool pexels_key_available() {
    static int avail = -1;
    if (avail < 0)
        avail = run_cmd_status("command -v secret-tool >/dev/null 2>&1") == 0 ? 1 : 0;
    return avail == 1;
}

static std::string key_lookup() {
    // Test-only override so end-to-end runs don't need the user's keyring.
    if (const char* k = getenv("PEXELS_API_KEY")) return k;
    FILE* p = popen("secret-tool lookup service pexels key api 2>/dev/null", "r");
    if (!p) return {};
    std::string out;
    char buf[512];
    while (fgets(buf, sizeof(buf), p)) out += buf;
    pclose(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

std::string pexels_key() { return key_lookup(); }

bool pexels_key_present() {
    // PEXELS_API_KEY counts: otherwise the settings UI claims "no key" while
    // the library works fine off the env override.
    if (const char* k = getenv("PEXELS_API_KEY")) return *k != '\0';
    return pexels_key_available() && !key_lookup().empty();
}

bool pexels_key_store(const std::string& key) {
    if (!pexels_key_available() || key.empty()) return false;
    FILE* p = popen("secret-tool store --label 'Pop Maker Studio Pexels' "
                    "service pexels key api 2>/dev/null", "w");
    if (!p) return false;
    fwrite(key.data(), 1, key.size(), p);
    return pclose(p) == 0;
}

bool pexels_key_clear() {
    if (!pexels_key_available()) return false;
    return run_cmd_status("secret-tool clear service pexels key api 2>/dev/null") == 0;
}

// ── Authenticated GET ─────────────────────────────────────────────────────────
// The Authorization header goes through a 0600 mkstemp'd curl --config file,
// never argv — argv is world-readable in /proc while curl runs. Body goes to a
// temp file; the HTTP code is written to stdout via -w. `diag` collects curl's
// stderr diagnostics (useful when the request never completes). Returns the
// HTTP status code (0 if curl itself failed to run).

static int pexels_api_get(const std::string& url, const std::string& key,
                          std::string& body, std::string& diag) {
    char cfg_tmpl[] = "/tmp/pms_pexels_cfg_XXXXXX";
    int cfd = mkstemp(cfg_tmpl);
    if (cfd < 0) return 0;
    fchmod(cfd, 0600);
    std::string cfg = "header = \"Authorization: " + key + "\"\n";
    ssize_t wr = write(cfd, cfg.data(), cfg.size());
    close(cfd);
    if (wr != (ssize_t)cfg.size()) { unlink(cfg_tmpl); return 0; }

    char body_tmpl[] = "/tmp/pms_pexels_body_XXXXXX";
    int bfd = mkstemp(body_tmpl);
    if (bfd < 0) { unlink(cfg_tmpl); return 0; }
    close(bfd);

    std::string cmd = "curl -sS -L --max-time 20 --config \"" + std::string(cfg_tmpl)
                    + "\" -o \"" + body_tmpl + "\" -w \"\\n%{http_code}\" \""
                    + url + "\" 2>&1";
    std::string out;
    FILE* p = popen(cmd.c_str(), "r");
    if (p) {
        char buf[4096];
        while (fgets(buf, sizeof(buf), p)) out += buf;
        pclose(p);
    }

    FILE* bf = fopen(body_tmpl, "rb");
    if (bf) {
        char buf[16384];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), bf)) > 0) body.append(buf, n);
        fclose(bf);
    }
    unlink(body_tmpl);
    unlink(cfg_tmpl);

    // Last line is the -w http code; anything before it is curl diagnostics.
    size_t nl = out.rfind('\n');
    std::string code = nl == std::string::npos ? std::string() : out.substr(nl + 1);
    while (!code.empty() && (code.back() == '\r' || code.back() == ' ')) code.pop_back();
    int http = 0;
    bool numeric = !code.empty();
    for (char c : code) if (c < '0' || c > '9') { numeric = false; break; }
    if (numeric)
        for (char c : code) http = http * 10 + (c - '0');

    diag = (nl != std::string::npos) ? out.substr(0, nl) : out;
    if (!diag.empty() && diag.back() == '\n') diag.pop_back();
    return http;
}

// ── Search ────────────────────────────────────────────────────────────────────
// json::value(key, default) THROWS type_error.302 when the field exists but is
// null (hls video_file entries have null width/fps) — so fields are read with
// null-guarded helpers instead.

static long long jget_i64(const json& o, const char* key) {
    auto it = o.find(key);
    return (it != o.end() && it->is_number()) ? it->get<long long>() : 0;
}

static int jget_int(const json& o, const char* key) {
    auto it = o.find(key);
    return (it != o.end() && it->is_number()) ? it->get<int>() : 0;
}

static double jget_dbl(const json& o, const char* key) {
    auto it = o.find(key);
    return (it != o.end() && it->is_number()) ? it->get<double>() : 0.0;
}

static std::string jget_str(const json& o, const char* key) {
    auto it = o.find(key);
    return (it != o.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

static bool parse_search_body(PexelsKind kind, const std::string& body,
                              std::vector<PexelsItem>& out, bool& has_more) {
    try {
        json j = json::parse(body);
        if (!j.is_object()) return false;
        has_more = !jget_str(j, "next_page").empty();

        if (kind == PexelsKind::Photo) {
            for (auto& ph : j.value("photos", json::array())) {
                if (!ph.is_object()) continue;
                PexelsItem it;
                it.id           = jget_i64(ph, "id");
                it.photographer = jget_str(ph, "photographer");
                it.alt          = jget_str(ph, "alt");
                it.width        = jget_int(ph, "width");
                it.height       = jget_int(ph, "height");
                auto src = ph.find("src");
                if (src != ph.end() && src->is_object()) {
                    it.thumb_url    = jget_str(*src, "small");
                    it.download_url = jget_str(*src, "large2x");
                }
                out.push_back(std::move(it));
            }
        } else {
            for (auto& v : j.value("videos", json::array())) {
                if (!v.is_object()) continue;
                PexelsItem it;
                it.id        = jget_i64(v, "id");
                it.duration  = jget_int(v, "duration");
                it.thumb_url = jget_str(v, "image");
                auto user = v.find("user");
                if (user != v.end() && user->is_object())
                    it.photographer = jget_str(*user, "name");

                // Best mp4 variant: largest width <= 1920, tie-break fps.
                int    best_w = -1;
                double best_fps = -1.0;
                for (auto& vf : v.value("video_files", json::array())) {
                    if (!vf.is_object()) continue;
                    if (jget_str(vf, "file_type") != "video/mp4") continue;
                    int w = jget_int(vf, "width");   // null (hls) → 0, skipped
                    if (w <= 0 || w > 1920) continue;
                    double fps = jget_dbl(vf, "fps");
                    if (w > best_w || (w == best_w && fps > best_fps)) {
                        best_w = w; best_fps = fps;
                        it.download_url = jget_str(vf, "link");
                        it.width  = w;
                        it.height = jget_int(vf, "height");
                    }
                }
                out.push_back(std::move(it));
            }
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void pexels_search(PexelsKind kind, const std::string& query,
                   const PexelsFilters& f, int page, PexelsSearch& s) {
    std::string key = pexels_key();
    if (key.empty()) {
        s.error = "No Pexels API key — add one in File → Settings";
        s.status.store(PexelsSearch::Status::Error, std::memory_order_release);
        return;
    }

    s.results.clear();
    s.error.clear();
    s.page = 0;
    s.has_more = false;
    s.status.store(PexelsSearch::Status::Running, std::memory_order_release);
    uint32_t my_gen = s.gen.load(std::memory_order_relaxed);

    std::thread([kind, query, f, page, key, my_gen, &s]() {
        if (g_shutdown.load(std::memory_order_relaxed)) return;

        std::string url = kind == PexelsKind::Photo
                            ? "https://api.pexels.com/v1/search"
                            : "https://api.pexels.com/v1/videos/search";
        url += "?query=" + url_encode(query);
        switch (f.orientation) {
            case PexelsFilters::OriLandscape: url += "&orientation=landscape"; break;
            case PexelsFilters::OriPortrait:  url += "&orientation=portrait";  break;
            case PexelsFilters::OriSquare:    url += "&orientation=square";    break;
            default: break;   // OriAny → no orientation param
        }
        url += "&per_page=15&page=" + std::to_string(page);
        // Neither endpoint documents an order/sort parameter — relevance order only.

        std::string body, diag;
        int http = pexels_api_get(url, key, body, diag);

        if (s.gen.load(std::memory_order_relaxed) != my_gen) return;  // cancelled

        if (http == 401) {
            s.error = "Invalid Pexels API key";
            s.status.store(PexelsSearch::Status::Error, std::memory_order_release);
            return;
        }
        if (http < 200 || http >= 300 || body.empty() || body.front() != '{') {
            std::string snippet = !body.empty() ? body : diag;
            if (snippet.size() > 120) snippet.resize(120);
            s.error = snippet.empty() ? "curl binary not found"
                                      : "Unexpected response: " + snippet;
            s.status.store(PexelsSearch::Status::Error, std::memory_order_release);
            return;
        }

        std::vector<PexelsItem> items;
        bool has_more = false;
        if (!parse_search_body(kind, body, items, has_more)) {
            std::string snippet = !body.empty() ? body : diag;
            if (snippet.size() > 120) snippet.resize(120);
            s.error = "Unexpected response: " + snippet;
            s.status.store(PexelsSearch::Status::Error, std::memory_order_release);
            return;
        }

        s.results = std::move(items);
        s.page = page;
        s.has_more = has_more;
        s.status.store(PexelsSearch::Status::Done, std::memory_order_release);
    }).detach();
}

void pexels_search_cancel(PexelsSearch& s) {
    s.gen.fetch_add(1, std::memory_order_relaxed);
    s.status.store(PexelsSearch::Status::Idle, std::memory_order_relaxed);
    s.results.clear();
}

// ── Downloads (user content → managed share dir) ──────────────────────────────

static std::string pexels_dir(PexelsKind kind) {
    const char* home = getenv("HOME");
    std::string base = (home && *home) ? std::string(home) : std::string("/tmp");
    return base + "/.local/share/pop-maker-studio/pexels/"
         + (kind == PexelsKind::Video ? "videos" : "photos");
}

// slug: lowercased photographer, non-alphanumerics collapsed to '-'
// (runs collapsed, trimmed; empty ⇒ "pexels")
static std::string make_slug(const std::string& photographer) {
    std::string slug;
    bool prev_dash = false;
    for (unsigned char c : photographer) {
        unsigned char lc = (unsigned char)tolower(c);
        if (isalnum(lc)) {
            slug += (char)lc;
            prev_dash = false;
        } else if (!slug.empty() && !prev_dash) {
            slug += '-';
            prev_dash = true;
        }
    }
    while (!slug.empty() && slug.back() == '-') slug.pop_back();
    if (slug.empty()) slug = "pexels";
    return slug;
}

std::string pexels_download_path(const PexelsItem& item, PexelsKind kind) {
    std::string dir = pexels_dir(kind);
    std::error_code ec;
    fs::create_directories(dir, ec);   // lazy
    char idbuf[32];
    snprintf(idbuf, sizeof(idbuf), "%lld", (long long)item.id);
    return dir + "/pexels-" + idbuf + "-" + make_slug(item.photographer)
         + (kind == PexelsKind::Video ? ".mp4" : ".jpg");
}

bool pexels_downloaded(const PexelsItem& item, PexelsKind kind) {
    std::error_code ec;
    return fs::exists(pexels_download_path(item, kind), ec);
}

void pexels_download(const PexelsItem& item, PexelsKind kind, PexelsDownload& dl) {
    dl.status.store(PexelsDownload::Status::Running, std::memory_order_release);
    dl.bytes_done.store(0, std::memory_order_relaxed);
    dl.bytes_total = 0;
    dl.out_path = pexels_download_path(item, kind);
    dl.error_msg.clear();

    std::thread([item, kind, &dl]() {
        if (g_shutdown.load(std::memory_order_relaxed)) return;
        const std::string tmp = dl.out_path + ".part";
        std::error_code ec;
        fs::create_directories(fs::path(dl.out_path).parent_path(), ec);

        // HEAD → Content-Length for the progress bar
        {
            std::string cmd = "curl -sI -L --max-time 10 \""
                            + item.download_url + "\" 2>/dev/null";
            FILE* hp = popen(cmd.c_str(), "r");
            if (hp) {
                char buf[256];
                while (fgets(buf, sizeof(buf), hp))
                    if (strncasecmp(buf, "content-length:", 15) == 0)
                        dl.bytes_total = (uint64_t)atoll(buf + 15);
                pclose(hp);
            }
        }

        if (g_shutdown.load(std::memory_order_relaxed)) return;

        // Stream to the .part file; poll() reports progress from its size.
        std::string cmd = "curl -L --max-time 600 --fail -o \"" + tmp
                        + "\" \"" + item.download_url + "\" 2>/dev/null";
        int rc = -1;
        FILE* p = popen(cmd.c_str(), "r");
        if (p) rc = pclose(p);
        bool ok = p && WIFEXITED(rc) && WEXITSTATUS(rc) == 0;

        if (!ok) {
            fs::remove(tmp, ec);
            dl.error_msg = "Download failed — media URL may be expired or unreachable";
            dl.status.store(PexelsDownload::Status::Error, std::memory_order_release);
            return;
        }

        std::error_code ec2;
        auto sz = fs::file_size(tmp, ec2);
        if (ec2 || sz == 0) {
            fs::remove(tmp, ec2);
            dl.error_msg = "Download failed — empty file";
            dl.status.store(PexelsDownload::Status::Error, std::memory_order_release);
            return;
        }

        if (g_shutdown.load(std::memory_order_relaxed)) {
            fs::remove(tmp, ec2);
            return;
        }

        fs::rename(tmp, dl.out_path, ec2);
        if (ec2) {
            dl.error_msg = "Could not move file: " + ec2.message();
            dl.status.store(PexelsDownload::Status::Error, std::memory_order_release);
            return;
        }
        dl.bytes_done.store(fs::file_size(dl.out_path, ec2), std::memory_order_relaxed);
        dl.status.store(PexelsDownload::Status::Done, std::memory_order_release);
    }).detach();
}

void pexels_download_poll(PexelsDownload& dl) {
    if (dl.status.load(std::memory_order_relaxed) != PexelsDownload::Status::Running)
        return;
    std::error_code ec;
    auto sz = fs::file_size(dl.out_path + ".part", ec);
    if (!ec) dl.bytes_done.store((uint64_t)sz, std::memory_order_relaxed);
}

// ── Thumbnail queue (regeneratable → media cache) ─────────────────────────────

struct ThumbReq { long long id; std::string url; };

struct ThumbState {
    std::mutex              mu;
    std::condition_variable cv;
    std::deque<ThumbReq>    queue;
    std::unordered_set<long long>   ids;    // queued + in-flight ids
    std::unordered_set<std::string> urls;   // queued + in-flight urls
    std::unordered_set<long long>   done;   // fetched successfully
    bool started = false;
};
// Intentionally leaked (process-lifetime singleton): the detached worker parks
// in cv.wait() while idle, and glibc's pthread_cond_destroy blocks until all
// waiters leave — destroying a file-static cv at exit with the worker parked
// would deadlock. Leaking mirrors the detached-thread teardown model: the OS
// reclaims everything at process exit.
static ThumbState* g_thumb = new ThumbState;
static const int kMaxThumbCurls = 4;

static void thumb_worker();

std::string pexels_thumb_path(long long id) {
    char idbuf[32];
    snprintf(idbuf, sizeof(idbuf), "%lld", id);
    return media_cache_dir() + "/pexels/thumbs/" + idbuf + ".jpg";
}

void pexels_thumb_fetch(long long id, const std::string& url) {
    if (url.empty()) return;
    std::lock_guard<std::mutex> lk(g_thumb->mu);
    if (g_thumb->done.count(id) || g_thumb->ids.count(id)) return;  // dedup by id
    if (g_thumb->urls.count(url)) return;                            // dedup by url
    g_thumb->ids.insert(id);
    g_thumb->urls.insert(url);
    g_thumb->queue.push_back({id, url});
    if (!g_thumb->started) {
        g_thumb->started = true;
        std::thread(thumb_worker).detach();
    }
    g_thumb->cv.notify_one();
}

static pid_t thumb_spawn(const ThumbReq& r) {
    std::string out = pexels_thumb_path(r.id);
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { dup2(dn, STDOUT_FILENO); dup2(dn, STDERR_FILENO); close(dn); }
        execlp("curl", "curl", "-sS", "--max-time", "30", "--fail",
               "-o", out.c_str(), r.url.c_str(), (char*)nullptr);
        _exit(127);
    }
    return pid;
}

static void thumb_worker() {
    std::error_code ec;
    fs::create_directories(fs::path(pexels_thumb_path(0)).parent_path(), ec);

    std::vector<std::tuple<pid_t, long long, std::string>> active;

    while (true) {
        // Reap finished children (non-blocking); release dedup reservations.
        for (auto it = active.begin(); it != active.end(); ) {
            int st = 0;
            pid_t r = waitpid(std::get<0>(*it), &st, WNOHANG);
            if (r == std::get<0>(*it)) {
                long long id = std::get<1>(*it);
                {
                    std::lock_guard<std::mutex> lk(g_thumb->mu);
                    g_thumb->ids.erase(id);
                    g_thumb->urls.erase(std::get<2>(*it));
                    // Terminal either way: a failed thumb stays missing (the
                    // card keeps its placeholder) instead of being re-queued
                    // every frame — the UI calls pexels_thumb_fetch() per
                    // frame for visible cards, so retrying would spawn a curl
                    // storm on a dead URL.
                    g_thumb->done.insert(id);
                }
                it = active.erase(it);
            } else ++it;
        }

        if (g_shutdown.load(std::memory_order_relaxed)) {
            for (auto& a : active) kill(std::get<0>(a), SIGTERM);
            break;
        }

        // Spawn up to 4 concurrent curls.
        {
            std::lock_guard<std::mutex> lk(g_thumb->mu);
            while (!g_thumb->queue.empty() && (int)active.size() < kMaxThumbCurls) {
                ThumbReq r = g_thumb->queue.front();
                g_thumb->queue.pop_front();
                pid_t pid = thumb_spawn(r);
                if (pid > 0) active.emplace_back(pid, r.id, r.url);
                else {
                    g_thumb->ids.erase(r.id);      // let it be retried
                    g_thumb->urls.erase(r.url);
                }
            }
        }

        if (g_thumb->queue.empty() && active.empty()) {
            std::unique_lock<std::mutex> lk(g_thumb->mu);
            g_thumb->cv.wait(lk, [] {
                return g_shutdown.load(std::memory_order_relaxed) ||
                       !g_thumb->queue.empty();
            });
        }
    }
}
