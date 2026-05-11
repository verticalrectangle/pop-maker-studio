// hf_api.cpp — HuggingFace search + download, curl binary only, no libraries

#include "hf_api.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <thread>
#include <filesystem>

namespace fs = std::filesystem;

// ── Helpers ───────────────────────────────────────────────────────────────────

static bool ends_with(const std::string& s, const char* suffix) {
    size_t sl = strlen(suffix);
    return s.size() >= sl && s.compare(s.size() - sl, sl, suffix) == 0;
}

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

// ── Minimal JSON field extraction ─────────────────────────────────────────────

static std::string jstr(const char* p, const char* key) {
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    const char* found = strstr(p, needle);
    if (!found) return {};
    found += strlen(needle);
    const char* end = found;
    while (*end && *end != '"') {
        if (*end == '\\') ++end;
        ++end;
    }
    std::string result;
    for (const char* c = found; c < end; ++c) {
        if (*c == '\\' && *(c + 1) == '/') { result += '/'; ++c; }
        else result += *c;
    }
    return result;
}

static long long jint(const char* p, const char* key) {
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char* found = strstr(p, needle);
    if (!found) return 0;
    found += strlen(needle);
    while (*found == ' ') ++found;
    return atoll(found);
}

// ── Parser ────────────────────────────────────────────────────────────────────

static std::vector<HFModel> parse_models(const std::string& json) {
    std::vector<HFModel> out;
    const char* p = json.c_str();

    while (true) {
        const char* mid = strstr(p, "\"modelId\":\"");
        if (!mid) break;

        const char* next = strstr(mid + 11, "\"modelId\":\"");
        std::string obj(mid, next ? (size_t)(next - mid) : strlen(mid));
        const char* op = obj.c_str();

        HFModel m;
        m.repo      = jstr(mid, "modelId");
        m.downloads = (int)jint(op, "downloads");

        // Scan siblings: prefer first .pth, fall back to first .zip
        const char* rp = op;
        std::string zip_fallback;
        while (true) {
            const char* rf = strstr(rp, "\"rfilename\":\"");
            if (!rf) break;
            rf += 13;
            const char* rfend = strchr(rf, '"');
            if (!rfend) break;
            std::string fname(rf, rfend - rf);
            if (ends_with(fname, ".pth")) {
                m.model_file = std::move(fname);
                break;
            }
            if (zip_fallback.empty() && ends_with(fname, ".zip"))
                zip_fallback = fname;
            rp = rfend + 1;
        }
        if (m.model_file.empty())
            m.model_file = std::move(zip_fallback);

        if (!m.repo.empty() && !m.model_file.empty())
            out.push_back(std::move(m));

        p = next ? next : mid + strlen(mid);
    }
    return out;
}

// ── Search ────────────────────────────────────────────────────────────────────

void hf_search(const std::string& query, HFSearch& s) {
    s.results.clear();
    s.error.clear();
    s.status.store(HFSearch::Status::Running, std::memory_order_release);
    uint32_t my_gen = s.gen.load(std::memory_order_relaxed);

    std::thread([query, my_gen, &s]() {
        // Append "rvc" so we always get RVC models; "filter=rvc" is a broken tag.
        std::string url = "https://huggingface.co/api/models?search="
                        + url_encode(query + " rvc")
                        + "&limit=20&full=true&sort=downloads&direction=-1";

        std::string cmd = "curl -sS --max-time 15 \"" + url + "\" 2>&1";
        FILE* p = popen(cmd.c_str(), "r");
        if (!p) {
            if (s.gen.load(std::memory_order_relaxed) == my_gen) {
                s.error = "curl binary not found";
                s.status.store(HFSearch::Status::Error, std::memory_order_release);
            }
            return;
        }

        std::string json;
        char buf[8192];
        while (fgets(buf, sizeof(buf), p))
            json += buf;
        pclose(p);

        if (s.gen.load(std::memory_order_relaxed) != my_gen) return;

        if (json.empty() || json.front() != '[') {
            if (json.size() > 120) json.resize(120);
            s.error = "Unexpected response: " + json;
            s.status.store(HFSearch::Status::Error, std::memory_order_release);
            return;
        }

        s.results = parse_models(json);
        s.status.store(HFSearch::Status::Done, std::memory_order_release);
    }).detach();
}

void hf_search_cancel(HFSearch& s) {
    s.gen.fetch_add(1, std::memory_order_relaxed);
    s.status.store(HFSearch::Status::Idle, std::memory_order_relaxed);
    s.results.clear();
}

// ── Cache helpers ─────────────────────────────────────────────────────────────

static std::string rvc_cache_dir() {
    const char* home = getenv("HOME");
    return home ? std::string(home) + "/.cache/pop-maker-studio/rvc/" : "";
}

std::string hf_rvc_model_path(const std::string& model_file) {
    std::string dir = rvc_cache_dir();
    if (dir.empty()) return {};
    // If model is a zip, the installed artifact is <stem>.pth
    if (ends_with(model_file, ".zip")) {
        std::string stem = model_file.substr(0, model_file.size() - 4);
        return dir + stem + ".pth";
    }
    return dir + model_file;
}

bool hf_rvc_installed(const std::string& model_file) {
    std::string p = hf_rvc_model_path(model_file);
    return !p.empty() && fs::exists(p);
}

// ── Download ──────────────────────────────────────────────────────────────────

void hf_download_poll(HFDownload& dl) {
    if (dl.status.load(std::memory_order_relaxed) != HFDownload::Status::Running)
        return;
    if (dl.tmp_path.empty()) return;
    std::error_code ec;
    auto sz = fs::file_size(dl.tmp_path, ec);
    if (!ec) dl.bytes_done.store((uint64_t)sz, std::memory_order_relaxed);
}

void hf_download_model(const std::string& repo, const std::string& model_file,
                       const std::string& out_path, HFDownload& dl) {
    dl.status.store(HFDownload::Status::Running, std::memory_order_release);
    dl.bytes_done.store(0, std::memory_order_relaxed);
    dl.bytes_total = 0;
    dl.out_path    = out_path;   // final .pth path
    dl.tmp_path    = out_path + ".dl";
    dl.error_msg.clear();

    std::error_code ec;
    fs::create_directories(fs::path(out_path).parent_path(), ec);

    bool is_zip = ends_with(model_file, ".zip");

    std::thread([repo, model_file, out_path, is_zip, &dl]() {
        std::string url = "https://huggingface.co/" + repo
                        + "/resolve/main/" + model_file;

        // HEAD → Content-Length for progress bar
        {
            std::string cmd = "curl -sI --max-time 10 --location \""
                            + url + "\" 2>/dev/null";
            FILE* hp = popen(cmd.c_str(), "r");
            if (hp) {
                char buf[256];
                while (fgets(buf, sizeof(buf), hp))
                    if (strncasecmp(buf, "content-length:", 15) == 0)
                        dl.bytes_total = (uint64_t)atoll(buf + 15);
                pclose(hp);
            }
        }

        // Download to .dl temp file
        std::string dl_cmd = "curl -L --max-time 600 --fail -o \""
                           + dl.tmp_path + "\" \"" + url + "\" 2>/dev/null";
        int rc = system(dl_cmd.c_str());

        if (rc != 0) {
            fs::remove(dl.tmp_path);
            dl.error_msg = "Download failed — model may be private or moved";
            dl.status.store(HFDownload::Status::Error, std::memory_order_release);
            return;
        }

        std::error_code ec2;
        auto sz = fs::file_size(dl.tmp_path, ec2);
        if (ec2 || sz < 1024) {
            fs::remove(dl.tmp_path);
            dl.error_msg = "File too small — likely an error page, not a model";
            dl.status.store(HFDownload::Status::Error, std::memory_order_release);
            return;
        }

        if (!is_zip) {
            // Plain .pth — move to final path
            fs::rename(dl.tmp_path, out_path, ec2);
            if (ec2) {
                dl.error_msg = "Could not move file: " + ec2.message();
                dl.status.store(HFDownload::Status::Error, std::memory_order_release);
                return;
            }
        } else {
            // Zip — extract .pth files into a temp dir, then pick one
            std::string zip_path = out_path + ".zip";
            fs::rename(dl.tmp_path, zip_path, ec2);

            std::string extract_dir = out_path + ".unzip_tmp";
            fs::create_directories(extract_dir, ec2);

            std::string unzip_cmd = "unzip -j -o \""
                                  + zip_path + "\" \"*.pth\" -d \""
                                  + extract_dir + "\" 2>/dev/null";
            rc = system(unzip_cmd.c_str());

            // Find extracted .pth
            std::string found_pth;
            if (rc == 0) {
                for (auto& entry : fs::directory_iterator(extract_dir, ec2)) {
                    if (entry.path().extension() == ".pth") {
                        found_pth = entry.path().string();
                        break;
                    }
                }
            }

            if (found_pth.empty()) {
                fs::remove(zip_path);
                fs::remove_all(extract_dir);
                dl.error_msg = "No .pth found inside zip";
                dl.status.store(HFDownload::Status::Error, std::memory_order_release);
                return;
            }

            fs::rename(found_pth, out_path, ec2);
            fs::remove(zip_path);
            fs::remove_all(extract_dir);

            if (ec2) {
                dl.error_msg = "Could not move extracted file: " + ec2.message();
                dl.status.store(HFDownload::Status::Error, std::memory_order_release);
                return;
            }
        }

        dl.bytes_done.store(fs::file_size(out_path, ec2), std::memory_order_relaxed);
        dl.status.store(HFDownload::Status::Done, std::memory_order_release);
    }).detach();
}
