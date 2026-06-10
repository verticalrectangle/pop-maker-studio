// agent_harness.cpp — in-app agent: OpenAI-compatible chat loop (DeepSeek by
// default) with tool calls dispatched to the app's own IPC socket. The app is
// a client of itself, so the in-app agent gets identical behavior to external
// MCP agents: auto-batch undo, quiet acks, the canvas agent pill, everything.
//
// Threading: one worker std::thread per user turn. The worker owns the wire
// history and appends display rows under s_mu; the UI thread snapshots rows
// per frame. HTTP is a curl child process (house style — ffmpeg and
// secret-tool are subprocesses too) so streaming reads are line-buffered SSE
// and Stop can SIGTERM the child.
#include "agent_harness.h"
#include "generated/agent_tools.h"
#include "json.hpp"

#include <atomic>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include "stb_image.h"
#include "stb_image_write.h"

using json = nlohmann::json;

// ── State ─────────────────────────────────────────────────────────────────────

static std::mutex            s_mu;
static std::vector<AgentRow> s_rows;
static json                  s_wire = json::array();  // OpenAI messages history
// Env overrides are dev/test conveniences (point at a mock or local server
// without touching settings): PMS_AGENT_BASE_URL, PMS_AGENT_MODEL.
static AgentConfig           s_cfg = [] {
    AgentConfig c;
    if (const char* u = getenv("PMS_AGENT_BASE_URL")) c.base_url = u;
    if (const char* m = getenv("PMS_AGENT_MODEL"))    c.model    = m;
    return c;
}();
static std::atomic<bool>     s_running{false};
static std::atomic<bool>     s_stop{false};
static std::atomic<pid_t>    s_curl_pid{0};
static std::thread           s_worker;

static const char* kSockPath   = "/tmp/pop-maker-studio.sock";
static const int   kMaxToolIters = 24;
static const size_t kToolResultCap = 8 * 1024;

// ── Display rows ──────────────────────────────────────────────────────────────

static void row_add(AgentRole role, std::string text, std::string detail = "") {
    std::lock_guard<std::mutex> lk(s_mu);
    s_rows.push_back({role, std::move(text), std::move(detail), false});
}

static void row_stream_begin() {
    std::lock_guard<std::mutex> lk(s_mu);
    s_rows.push_back({AgentRole::Assistant, "", "", true});
}

static void row_stream_append(const std::string& delta) {
    std::lock_guard<std::mutex> lk(s_mu);
    if (!s_rows.empty() && s_rows.back().streaming) s_rows.back().text += delta;
}

static void row_stream_end() {
    std::lock_guard<std::mutex> lk(s_mu);
    if (!s_rows.empty() && s_rows.back().streaming) {
        s_rows.back().streaming = false;
        if (s_rows.back().text.empty()) s_rows.pop_back();
    }
}

// ── Tool table (generated from server.py, filtered to IPC-servable) ──────────

struct ToolInfo { json decl; bool has_quiet = false; };

static const std::vector<std::pair<std::string, ToolInfo>>& tool_table() {
    static std::vector<std::pair<std::string, ToolInfo>> table = [] {
        std::vector<std::pair<std::string, ToolInfo>> t;
        json all = json::parse(AGENT_TOOLS_JSON);
        for (auto& tool : all) {
            if (!tool.value("ipc", false)) continue;
            ToolInfo info;
            info.decl = {
                {"type", "function"},
                {"function", {
                    {"name",        tool["name"]},
                    {"description", tool["description"]},
                    {"parameters",  tool["inputSchema"]},
                }},
            };
            auto& schema = tool["inputSchema"];
            info.has_quiet = schema.contains("properties") &&
                             schema["properties"].contains("quiet");
            t.push_back({tool["name"].get<std::string>(), std::move(info)});
        }
        return t;
    }();
    return table;
}

static json tools_decl_json() {
    json arr = json::array();
    for (auto& [name, info] : tool_table()) arr.push_back(info.decl);
    return arr;
}

// ── IPC self-client ───────────────────────────────────────────────────────────
// One connection per call, single JSON-RPC line each way — same protocol the
// MCP server speaks. Served by ipc_server_poll on the main thread.

static bool ipc_request(const std::string& method, const json& params,
                        json& result, std::string& err) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { err = "socket() failed"; return false; }
    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, kSockPath, sizeof(addr.sun_path) - 1);
    if (connect(fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
        close(fd); err = "cannot connect to the editor IPC socket"; return false;
    }
    json req = {{"jsonrpc", "2.0"}, {"id", "agent"},
                {"method", method}, {"params", params}};
    std::string line = req.dump();
    line += "\n";
    size_t off = 0;
    while (off < line.size()) {
        ssize_t n = write(fd, line.data() + off, line.size() - off);
        if (n <= 0) { close(fd); err = "IPC write failed"; return false; }
        off += (size_t)n;
    }
    std::string buf;
    char chunk[65536];
    while (buf.empty() || buf.back() != '\n') {
        ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n <= 0) break;
        buf.append(chunk, (size_t)n);
    }
    close(fd);
    if (buf.empty()) { err = "empty IPC response"; return false; }
    json resp = json::parse(buf, nullptr, false);
    if (resp.is_discarded()) { err = "unparseable IPC response"; return false; }
    if (resp.contains("error")) {
        err = resp["error"].is_string() ? resp["error"].get<std::string>()
                                        : resp["error"].dump();
        return false;
    }
    result = resp.value("result", json::object());
    return true;
}

// ── Image helpers (vision) ────────────────────────────────────────────────────

static std::string b64_encode(const unsigned char* data, size_t len) {
    static const char* tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3) {
        unsigned v = data[i] << 16;
        if (i + 1 < len) v |= data[i + 1] << 8;
        if (i + 2 < len) v |= data[i + 2];
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += (i + 1 < len) ? tbl[(v >> 6) & 63] : '=';
        out += (i + 2 < len) ? tbl[v & 63] : '=';
    }
    return out;
}

// Load a PNG/JPG, box-downscale so the longest edge ≤ max_edge, re-encode to
// JPEG in memory (much smaller token footprint than PNG for photos), return a
// data: URL. Empty string on failure.
static std::string image_file_to_data_url(const std::string& path, int max_edge) {
    int w = 0, h = 0, ch = 0;
    unsigned char* px = stbi_load(path.c_str(), &w, &h, &ch, 3);
    if (!px || w <= 0 || h <= 0) { if (px) stbi_image_free(px); return ""; }

    int ow = w, oh = h;
    if (w > max_edge || h > max_edge) {
        float s = (float)max_edge / (float)(w > h ? w : h);
        ow = (int)(w * s); oh = (int)(h * s);
        if (ow < 1) ow = 1;
        if (oh < 1) oh = 1;
    }
    std::vector<unsigned char> small((size_t)ow * oh * 3);
    for (int y = 0; y < oh; ++y) {
        int sy0 = y * h / oh, sy1 = (y + 1) * h / oh;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int x = 0; x < ow; ++x) {
            int sx0 = x * w / ow, sx1 = (x + 1) * w / ow;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            unsigned r = 0, g = 0, b = 0, n = 0;
            for (int sy = sy0; sy < sy1; ++sy)
                for (int sx = sx0; sx < sx1; ++sx) {
                    const unsigned char* p = px + ((size_t)sy * w + sx) * 3;
                    r += p[0]; g += p[1]; b += p[2]; ++n;
                }
            unsigned char* d = small.data() + ((size_t)y * ow + x) * 3;
            d[0] = (unsigned char)(r / n);
            d[1] = (unsigned char)(g / n);
            d[2] = (unsigned char)(b / n);
        }
    }
    stbi_image_free(px);

    std::vector<unsigned char> jpg;
    stbi_write_jpg_to_func(
        [](void* ctx, void* data, int size) {
            auto* v = (std::vector<unsigned char>*)ctx;
            v->insert(v->end(), (unsigned char*)data, (unsigned char*)data + size);
        },
        &jpg, ow, oh, 3, small.data(), 70);
    if (jpg.empty()) return "";
    return "data:image/jpeg;base64," + b64_encode(jpg.data(), jpg.size());
}

// ── curl child ────────────────────────────────────────────────────────────────

struct CurlStream { FILE* out = nullptr; pid_t pid = 0; std::string hdr_path, body_path; };

static bool curl_begin(const std::string& url, const std::string& api_key,
                       const std::string& body, CurlStream& cs) {
    char hdr_tmpl[]  = "/tmp/pms_agent_hdr_XXXXXX";
    char body_tmpl[] = "/tmp/pms_agent_body_XXXXXX";
    int hfd = mkstemp(hdr_tmpl);
    int bfd = mkstemp(body_tmpl);
    if (hfd < 0 || bfd < 0) { if (hfd >= 0) close(hfd); if (bfd >= 0) close(bfd); return false; }
    fchmod(hfd, 0600); fchmod(bfd, 0600);
    // The Authorization header goes through a 0600 config file, never argv —
    // argv is world-readable in /proc while curl runs.
    std::string hdr = "header = \"Authorization: Bearer " + api_key + "\"\n";
    if (write(hfd, hdr.data(), hdr.size()) != (ssize_t)hdr.size()) { /* fallthrough */ }
    if (write(bfd, body.data(), body.size()) != (ssize_t)body.size()) { /* fallthrough */ }
    close(hfd); close(bfd);
    cs.hdr_path = hdr_tmpl; cs.body_path = body_tmpl;

    int pipefd[2];
    if (pipe(pipefd) != 0) return false;
    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return false; }
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]); close(pipefd[1]);
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { dup2(dn, STDERR_FILENO); close(dn); }
        std::string data_arg = "@" + cs.body_path;
        execlp("curl", "curl", "-sN", "--no-buffer",
               "--max-time", "600",
               "--config", cs.hdr_path.c_str(),
               "-H", "Content-Type: application/json",
               "--data-binary", data_arg.c_str(),
               url.c_str(), (char*)nullptr);
        _exit(127);
    }
    close(pipefd[1]);
    cs.out = fdopen(pipefd[0], "r");
    cs.pid = pid;
    s_curl_pid = pid;
    return cs.out != nullptr;
}

static void curl_end(CurlStream& cs) {
    if (cs.out) fclose(cs.out);
    if (cs.pid > 0) { int st = 0; waitpid(cs.pid, &st, 0); }
    s_curl_pid = 0;
    if (!cs.hdr_path.empty())  unlink(cs.hdr_path.c_str());
    if (!cs.body_path.empty()) unlink(cs.body_path.c_str());
    cs = {};
}

// ── Secret Service (secret-tool) ──────────────────────────────────────────────

static int run_cmd_status(const std::string& cmd) {
    int rc = system(cmd.c_str());
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

bool agent_key_available() {
    static int avail = -1;
    if (avail < 0)
        avail = run_cmd_status("command -v secret-tool >/dev/null 2>&1") == 0 ? 1 : 0;
    return avail == 1;
}

static std::string key_lookup() {
    // Test-only override so end-to-end runs don't need the user's keyring.
    if (const char* k = getenv("PMS_AGENT_KEY")) return k;
    FILE* p = popen("secret-tool lookup service pms-agent key api 2>/dev/null", "r");
    if (!p) return "";
    std::string out;
    char buf[512];
    while (fgets(buf, sizeof(buf), p)) out += buf;
    pclose(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

bool agent_key_present() { return agent_key_available() && !key_lookup().empty(); }

bool agent_key_store(const std::string& key) {
    if (!agent_key_available() || key.empty()) return false;
    FILE* p = popen("secret-tool store --label 'Pop Maker Studio Agent' "
                    "service pms-agent key api 2>/dev/null", "w");
    if (!p) return false;
    fwrite(key.data(), 1, key.size(), p);
    return pclose(p) == 0;
}

bool agent_key_clear() {
    if (!agent_key_available()) return false;
    return run_cmd_status("secret-tool clear service pms-agent key api 2>/dev/null") == 0;
}

// ── Tool execution ────────────────────────────────────────────────────────────

static std::string truncate_result(std::string s) {
    if (s.size() <= kToolResultCap) return s;
    s.resize(kToolResultCap);
    s += "\n…[truncated — re-query with a slimmer tool if you need more]";
    return s;
}

// Returns tool-role content (always a text string for API compatibility).
// extra_wire_msgs: appended AFTER the tool_result — used for snapshot images
// that must trail the tool_call_id response to keep the wire legal everywhere.
static json exec_tool(const std::string& name, json args,
                      std::vector<json>& extra_wire_msgs, bool vision) {
    // Default to small acks — the model can re-read state explicitly.
    const ToolInfo* info = nullptr;
    for (auto& [n, i] : tool_table()) if (n == name) { info = &i; break; }
    if (!info) return "error: unknown tool '" + name + "' (not servable in-app)";
    if (info->has_quiet && !args.contains("quiet")) args["quiet"] = true;

    json result; std::string err;
    if (!ipc_request(name, args, result, err))
        return "error: " + err;

    // take_snapshot is async: poll status, attach image as trailing user msg.
    if (name == "take_snapshot") {
        for (int i = 0; i < 50 && !s_stop; ++i) {
            usleep(200 * 1000);
            json st; std::string serr;
            if (!ipc_request("get_snapshot_status", json::object(), st, serr))
                return "error: " + serr;
            if (st.value("done", false)) {
                if (st.contains("error"))
                    return "error: " + st["error"].get<std::string>();
                std::string path = st.value("path", "");
                std::string note = "snapshot saved to " + path;
                if (vision && !path.empty()) {
                    std::string url = image_file_to_data_url(path, 512);
                    if (!url.empty()) {
                        row_add(AgentRole::Image, path);
                        extra_wire_msgs.push_back({
                            {"role", "user"},
                            {"content", json::array({
                                {{"type", "text"},
                                 {"text", "[tool output image: take_snapshot]"}},
                                {{"type", "image_url"},
                                 {"image_url", {{"url", url}}}},
                            })},
                        });
                    }
                }
                return note;
            }
        }
        return "error: snapshot timed out";
    }
    return truncate_result(result.dump());
}

// ── The chat loop ─────────────────────────────────────────────────────────────

static const char* kSystemPrompt =
    "You are the embedded editing agent inside Pop Maker Studio, a desktop "
    "video editor. You edit the project the user currently has open by "
    "calling tools; the user watches the canvas and timeline update live. "
    "Track 0 is the top/foreground layer. Times are in seconds. Mutations "
    "return small acks; call get_project or get_clips when you need to read "
    "state. take_snapshot shows you the canvas. Be concise in prose — do the "
    "work with tools and summarize briefly. You can put videos and images"
    "on the timeline.";;

static void worker_turn() {
    std::string api_key = key_lookup();
    if (api_key.empty()) {
        row_add(AgentRole::Error,
                "No API key. Add one in File \xe2\x86\x92 Settings.");
        s_running = false;
        return;
    }

    AgentConfig cfg;
    { std::lock_guard<std::mutex> lk(s_mu); cfg = s_cfg; }
    std::string url = cfg.base_url;
    while (!url.empty() && url.back() == '/') url.pop_back();
    url += "/chat/completions";

    static const json tools_decl = tools_decl_json();

    for (int iter = 0; iter < kMaxToolIters && !s_stop; ++iter) {
        json body;
        {
            std::lock_guard<std::mutex> lk(s_mu);
            body = {{"model", cfg.model}, {"messages", s_wire},
                    {"tools", tools_decl}, {"stream", true}};
        }

        CurlStream cs;
        if (!curl_begin(url, api_key, body.dump(), cs)) {
            row_add(AgentRole::Error, "failed to start curl");
            break;
        }

        // ── SSE stream: accumulate content + tool_call deltas ────────────────
        std::string content;
        std::string finish;
        json calls = json::array();   // accumulated by index
        std::string err_body;         // non-SSE error response
        bool stream_started = false;
        char* lineptr = nullptr; size_t cap = 0; ssize_t n;
        while ((n = getline(&lineptr, &cap, cs.out)) > 0) {
            if (s_stop) { kill(cs.pid, SIGTERM); break; }
            std::string line(lineptr, (size_t)n);
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
                line.pop_back();
            if (line.empty()) continue;
            if (line.rfind("data:", 0) != 0) { err_body += line; continue; }
            std::string payload = line.substr(5);
            if (!payload.empty() && payload[0] == ' ') payload.erase(0, 1);
            if (payload == "[DONE]") break;
            json ev = json::parse(payload, nullptr, false);
            if (ev.is_discarded() || !ev.contains("choices") ||
                ev["choices"].empty()) continue;
            auto& choice = ev["choices"][0];
            if (choice.contains("finish_reason") &&
                !choice["finish_reason"].is_null())
                finish = choice["finish_reason"].get<std::string>();
            if (!choice.contains("delta")) continue;
            auto& delta = choice["delta"];
            if (delta.contains("content") && delta["content"].is_string()) {
                std::string piece = delta["content"].get<std::string>();
                if (!piece.empty()) {
                    if (!stream_started) { row_stream_begin(); stream_started = true; }
                    content += piece;
                    row_stream_append(piece);
                }
            }
            if (delta.contains("tool_calls")) {
                for (auto& tc : delta["tool_calls"]) {
                    size_t idx = tc.value("index", 0);
                    while (calls.size() <= idx)
                        calls.push_back({{"id", ""},
                                         {"type", "function"},
                                         {"function", {{"name", ""},
                                                       {"arguments", ""}}}});
                    auto& slot = calls[idx];
                    if (tc.contains("id") && tc["id"].is_string())
                        slot["id"] = tc["id"];
                    if (tc.contains("function")) {
                        auto& f = tc["function"];
                        if (f.contains("name") && f["name"].is_string())
                            slot["function"]["name"] =
                                slot["function"]["name"].get<std::string>() +
                                f["name"].get<std::string>();
                        if (f.contains("arguments") && f["arguments"].is_string())
                            slot["function"]["arguments"] =
                                slot["function"]["arguments"].get<std::string>() +
                                f["arguments"].get<std::string>();
                    }
                }
            }
        }
        free(lineptr);
        curl_end(cs);
        if (stream_started) row_stream_end();
        if (s_stop) break;

        if (!err_body.empty() && content.empty() && calls.empty()) {
            // Probably an HTTP error JSON ({"error": {...}})
            row_add(AgentRole::Error, "API error: " + err_body.substr(0, 400));
            break;
        }

        // ── Append assistant message to the wire ──────────────────────────────
        json amsg = {{"role", "assistant"}};
        amsg["content"] = content.empty() ? json(nullptr) : json(content);
        if (!calls.empty()) amsg["tool_calls"] = calls;
        {
            std::lock_guard<std::mutex> lk(s_mu);
            s_wire.push_back(amsg);
        }

        if (calls.empty() || finish == "stop") break;

        // ── Execute tool calls serially ───────────────────────────────────────
        for (auto& call : calls) {
            if (s_stop) break;
            std::string tname = call["function"]["name"].get<std::string>();
            std::string targs_s = call["function"]["arguments"].get<std::string>();
            json targs = json::parse(targs_s.empty() ? "{}" : targs_s,
                                     nullptr, false);
            json content;
            std::vector<json> extra;
            std::string result_str;
            if (targs.is_discarded()) {
                result_str = "error: tool arguments were not valid JSON";
                content    = result_str;
            } else {
                content = exec_tool(tname, targs, extra, cfg.vision);
                if (content.is_string()) {
                    result_str = content.get<std::string>();
                } else if (content.is_array()) {
                    for (auto& part : content) {
                        if (part.value("type", "") == "text") {
                            result_str = part.value("text", "");
                            break;
                        }
                    }
                    if (result_str.empty()) result_str = content.dump();
                } else {
                    result_str = content.dump();
                }
            }
            std::string summary = "\xe2\x96\xb8 " + tname + " " +
                (targs_s.size() > 120 ? targs_s.substr(0, 117) + "…" : targs_s);
            bool failed = result_str.rfind("error:", 0) == 0;
            row_add(failed ? AgentRole::Error : AgentRole::Tool,
                    summary + (failed ? "  \xe2\x9c\x97" : ""),
                    "args: " + targs_s + "\n\nresult: " + result_str);
            std::lock_guard<std::mutex> lk(s_mu);
            s_wire.push_back({{"role", "tool"},
                              {"tool_call_id", call["id"]},
                              {"content", content}});
            for (auto& m : extra) s_wire.push_back(m);
        }
        if (iter == kMaxToolIters - 1)
            row_add(AgentRole::Error, "tool budget exhausted for this turn");
    }

    if (s_stop) row_add(AgentRole::Info, "(stopped)");
    s_running = false;
}

// ── Public API ────────────────────────────────────────────────────────────────

void agent_send(const std::string& user_text) {
    if (s_running || user_text.empty()) return;
    {
        std::lock_guard<std::mutex> lk(s_mu);
        if (s_wire.empty())
            s_wire.push_back({{"role", "system"}, {"content", kSystemPrompt}});
        s_wire.push_back({{"role", "user"}, {"content", user_text}});
        s_rows.push_back({AgentRole::User, user_text, "", false});
    }
    s_stop    = false;
    s_running = true;
    if (s_worker.joinable()) s_worker.join();
    s_worker = std::thread(worker_turn);
}

bool agent_running() { return s_running; }

void agent_stop() {
    if (!s_running) return;
    s_stop = true;
    pid_t pid = s_curl_pid;
    if (pid > 0) kill(pid, SIGTERM);
}

std::vector<AgentRow> agent_rows_snapshot() {
    std::lock_guard<std::mutex> lk(s_mu);
    return s_rows;
}

void agent_clear() {
    if (s_running) return;
    std::lock_guard<std::mutex> lk(s_mu);
    s_rows.clear();
    s_wire = json::array();
}

void agent_shutdown() {
    agent_stop();
    if (s_worker.joinable()) s_worker.join();
}

AgentConfig agent_get_config() {
    std::lock_guard<std::mutex> lk(s_mu);
    return s_cfg;
}

void agent_set_config(const AgentConfig& cfg) {
    std::lock_guard<std::mutex> lk(s_mu);
    s_cfg = cfg;
}
