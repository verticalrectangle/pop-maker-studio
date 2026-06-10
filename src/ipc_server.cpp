#include "ipc_server.h"
#include "render.h"
#include "bg_remove.h"
#include "history.h"
#include "runtime_fx.h"
#include "ui/pipeline.h"
#include "ui/panel_animation.h"
#include "ui/panel_media.h"
#include "ui/studio_shared.h"
#include "project.h"
#include "beat_detect.h"
#include "scene_detect.h"
#include "vision_caption.h"
#include "vision_download.h"
#include "video.h"
#include "proxy.h"
#include "transcribe.h"
#include "generated/fx_clip_set_dispatch.h"
#include "generated/fx_type_list.h"
#include "json.hpp"

static const char* k_gen_fx_names[] = {
#include "generated/fx_gen_names.h"
};

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <fstream>
#include <filesystem>
#include <limits>

using json = nlohmann::json;

// ── Static state ──────────────────────────────────────────────────────────────

static struct {
    std::atomic<bool> running{false};
    std::atomic<bool> done{false};
    BeatResult        result;
} s_audio_analysis;

static struct {
    std::atomic<bool> running{false};
    std::atomic<bool> done{false};
    std::string       sidecar_path;
    std::string       error;
} s_scene_analysis;

static int  g_srv_fd   = -1;
static std::string g_sock_path;

struct Client {
    int  fd = -1;
    std::string rbuf;  // accumulates partial reads
};
static std::vector<Client> g_clients;

// Batch state: explicit batch wraps multiple edits into one undo step.
// Auto-batch: single mutations without an explicit batch get one automatically.
static bool        g_in_batch    = false;
static bool        g_auto_batched = false;   // true when batch was opened implicitly
static std::string g_batch_label;
static AppState*   g_batch_state = nullptr;

// ── Async progress streaming ──────────────────────────────────────────────────
// Long-running ops mark the client fd "busy", stream {"type":"progress"} lines,
// then send the final {"id":…,"result":…} line and clear busy.
// process_client() skips busy fds so it doesn't interleave reads.

static std::mutex           g_busy_mtx;
static std::unordered_set<int> g_busy_fds;

static void fd_mark_busy(int fd) { std::lock_guard<std::mutex> lk(g_busy_mtx); g_busy_fds.insert(fd); }
static void fd_mark_free(int fd) { std::lock_guard<std::mutex> lk(g_busy_mtx); g_busy_fds.erase(fd); }
static bool fd_is_busy  (int fd) { std::lock_guard<std::mutex> lk(g_busy_mtx); return g_busy_fds.count(fd) > 0; }

// ── Agent activity indicator ──────────────────────────────────────────────────
// Tracks which AppState to clear when a non-batch IPC call finishes.
// Batch calls manage agent_active themselves via begin_batch/end_batch.
static std::atomic<AppState*> g_agent_ptr{nullptr};

static void agent_begin(AppState& state, const std::string& method) {
    if (g_in_batch) return;
    g_agent_ptr.store(&state);
    state.agent_active = true;
    state.agent_msg    = method;
}

static void agent_done() {
    if (g_in_batch) return;
    AppState* s = g_agent_ptr.exchange(nullptr);
    if (s) { s->agent_active = false; s->agent_msg.clear(); }
}

// ── Socket helpers ────────────────────────────────────────────────────────────

static void set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void send_json(int fd, const json& j) {
    std::string s = j.dump() + "\n";
    (void)write(fd, s.c_str(), s.size());
}

// id will be added by the caller at dispatch time
static void send_ok_id(int fd, const std::string& id, const json& result = json::object()) {
    json r; r["id"] = id; r["result"] = result;
    send_json(fd, r);
}

static void send_err_id(int fd, const std::string& id, const std::string& msg) {
    json r; r["id"] = id; r["error"] = msg;
    send_json(fd, r);
}

static void send_progress(int fd, const std::string& id, float progress, const std::string& message = "") {
    json j;
    j["id"]       = id;
    j["type"]     = "progress";
    j["progress"] = progress;
    if (!message.empty()) j["message"] = message;
    send_json(fd, j);
}

// MCP sends numeric values as JSON strings when the schema type is "{}".
// These helpers coerce either form to the target type.
static float jval_float(const json& v) {
    if (v.is_number()) return v.get<float>();
    if (v.is_string()) return std::stof(v.get<std::string>());
    return 0.f;
}
static int jval_int(const json& v) {
    if (v.is_number()) return v.get<int>();
    if (v.is_string()) return std::stoi(v.get<std::string>());
    return 0;
}
// snap_to_frame / snap_end_to_frame defined in ui/studio_shared.h
static bool jval_bool(const json& v) {
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number())  return v.get<int>() != 0;
    if (v.is_string()) { auto s = v.get<std::string>(); return s=="true"||s=="1"; }
    return false;
}

// ── AppState → JSON serialization ─────────────────────────────────────────────

static std::string clip_type_str(ClipType t) {
    switch (t) {
        case ClipType::Audio:      return "audio";
        case ClipType::Video:      return "video";
        case ClipType::Text:       return "text";
        case ClipType::Lyrics:     return "lyrics";
        case ClipType::Subtitle:   return "subtitle";
        case ClipType::Effect:     return "effect";
        case ClipType::Background: return "background";
        case ClipType::BodyFX:     return "body_fx";
        case ClipType::MultiFX:    return "multi_fx";
    }
    return "unknown";
}

static AnimStyle parse_anim_style(const std::string& s) {
    if (s == "fade")       return AnimStyle::Fade;
    if (s == "glitch")     return AnimStyle::Glitch;
    if (s == "typewriter") return AnimStyle::Typewriter;
    if (s == "bounce")     return AnimStyle::Bounce;
    if (s == "scale")      return AnimStyle::Scale;
    if (s == "slide")      return AnimStyle::Slide;
    if (s == "stack")      return AnimStyle::Stack;
    if (s == "block")      return AnimStyle::Block;
    return AnimStyle::None;
}

static FXType parse_fx_type(const std::string& s) {
    if (s == "grade")      return FXType::Grade;
    if (s == "blur")       return FXType::Blur;
    if (s == "vignette")   return FXType::Vignette;
    if (s == "glitch")     return FXType::Glitch;
    if (s == "zoom_punch") return FXType::ZoomPunch;
    if (s == "lut")        return FXType::LUT;
    if (s == "light_leak") return FXType::LightLeak;
    if (s == "vhs")        return FXType::VHS;
    if (s == "datamosh")   return FXType::Datamosh;
    if (s == "chroma_key") return FXType::ChromaKey;
    if (s == "ken_burns")  return FXType::KenBurns;
    for (int i = 0; i < k_gen_fx_count; ++i)
        if (s == k_gen_fx_names[i]) return k_gen_fx_types[i];
    return FXType::Grade;
}

// Set fx-specific params on an Effect clip from a string-keyed params object.
// fx_id is the snake_case type name (e.g. "mirror_tunnel") used to dispatch shader params.
static bool apply_effect_params(Clip& cl, const json& params,
                                const std::string& fx_id, std::string& err) {
    for (auto& [k, v] : params.items()) {
        if (!v.is_number()) { err = "param '" + k + "' must be a number"; return false; }
        float fv = v.get<float>();
        // Grade
        if      (k == "brightness")              { cl.fx_color_on = true; cl.fx_brightness  = fv; }
        else if (k == "contrast")                { cl.fx_color_on = true; cl.fx_contrast    = fv; }
        else if (k == "saturation")              { cl.fx_color_on = true; cl.fx_saturation  = fv; }
        else if (k == "hue")                     { cl.fx_color_on = true; cl.fx_hue         = fv; }
        // Blur
        else if (k == "blur")                    { cl.fx_blur_on  = true; cl.fx_blur        = fv; }
        // Vignette
        else if (k == "vignette")                { cl.fx_vignette_on = true; cl.fx_vignette = fv; }
        // Text override
        else if (k == "opacity_mul")             { cl.fx_text_on = true; cl.fx_opacity_mul  = fv; }
        else if (k == "scale_mul")               { cl.fx_text_on = true; cl.fx_scale_mul    = fv; }
        // Glitch
        else if (k == "glitch_chroma")           { cl.fx_glitch_chroma           = fv; }
        else if (k == "glitch_jitter")           { cl.fx_glitch_jitter           = fv; }
        else if (k == "glitch_corruption")       { cl.fx_glitch_corruption       = fv; }
        else if (k == "glitch_corruption_bleed") { cl.fx_glitch_corruption_bleed = fv; }
        // ZoomPunch
        else if (k == "zoom_strength")           { cl.fx_zoom_strength = fv; }
        else if (k == "zoom_decay")              { cl.fx_zoom_decay    = fv; }
        else if (k == "zoom_shake")              { cl.fx_zoom_shake    = fv; }
        // LightLeak
        else if (k == "leak_intensity")          { cl.fx_leak_intensity = fv; }
        else if (k == "leak_speed")              { cl.fx_leak_speed     = fv; }
        // VHS
        else if (k == "vhs_noise")               { cl.fx_vhs_noise    = fv; }
        else if (k == "vhs_bleed")               { cl.fx_vhs_bleed    = fv; }
        else if (k == "vhs_tracking")            { cl.fx_vhs_tracking = fv; }
        // Datamosh
        else if (k == "datamosh_intensity")      { cl.fx_datamosh_intensity = fv; }
        else if (k == "datamosh_spread")         { cl.fx_datamosh_spread    = fv; }
        // ChromaKey
        else if (k == "chroma_key_r")            { cl.fx_chroma_key_r         = fv; }
        else if (k == "chroma_key_g")            { cl.fx_chroma_key_g         = fv; }
        else if (k == "chroma_key_b")            { cl.fx_chroma_key_b         = fv; }
        else if (k == "chroma_key_threshold")    { cl.fx_chroma_key_threshold = fv; }
        else if (k == "chroma_key_softness")     { cl.fx_chroma_key_softness  = fv; }
        // All generated shader FX params — dispatched by fx_id + param name
        else if (!fx_clip_set_param(cl, fx_id, k, fv)) {
            err = "unknown param '" + k + "' for fx_type '" + fx_id + "'";
            return false;
        }
    }
    return true;
}

static std::string anim_style_str(AnimStyle s) {
    switch (s) {
        case AnimStyle::None:       return "none";
        case AnimStyle::Fade:       return "fade";
        case AnimStyle::Glitch:     return "glitch";
        case AnimStyle::Typewriter: return "typewriter";
        case AnimStyle::Bounce:     return "bounce";
        case AnimStyle::Scale:      return "scale";
        case AnimStyle::Slide:      return "slide";
        case AnimStyle::Stack:      return "stack";
        case AnimStyle::Block:      return "block";
        default:                    return "unknown";
    }
}

static json clip_to_json_slim(int idx, const Clip& c) {
    json j;
    j["index"]    = idx;
    j["type"]     = clip_type_str(c.clip_type);
    j["start"]    = c.start;
    j["end"]      = c.end;
    j["duration"] = c.end - c.start;
    j["in_point"] = c.in_point;
    if (!c.source_id.empty()) j["source"] = c.source_id;
    if (!c.text.empty())      j["text"]   = c.text;
    return j;
}

static json clip_to_json(int idx, const Clip& c) {
    json j;
    j["index"]       = idx;
    j["type"]        = clip_type_str(c.clip_type);
    j["start"]       = c.start;
    j["end"]         = c.end;
    j["text"]        = c.text;
    j["in_point"]    = c.in_point;
    j["duration"]    = c.end - c.start;
    j["volume"]      = c.volume;
    j["speed"]       = c.speed;
    j["opacity"]     = c.opacity;
    j["muted"]       = c.muted;
    j["clip_style"]  = anim_style_str(c.clip_style);
    j["font_size"]   = c.font_size;
    j["karaoke"]     = c.karaoke;
    j["sub_pos"]     = c.sub_pos;
    j["sub_pos_x"]   = c.sub_pos_x;
    j["sub_pos_y"]   = c.sub_pos_y;
    j["sub_anchor_h"]= c.sub_anchor_h;
    j["sub_wrap_w"]  = c.sub_wrap_w;
    j["sub_color"]   = {c.sub_color[0], c.sub_color[1], c.sub_color[2], c.sub_color[3]};
    j["karaoke_highlight_color"] = {c.karaoke_highlight_color[0], c.karaoke_highlight_color[1],
                                    c.karaoke_highlight_color[2], c.karaoke_highlight_color[3]};
    // TextStyle
    json ts;
    ts["shadow_enabled"] = c.ts.shadow_enabled;
    ts["shadow_ox"]      = c.ts.shadow_ox;
    ts["shadow_oy"]      = c.ts.shadow_oy;
    ts["shadow_col"]     = {c.ts.shadow_col[0], c.ts.shadow_col[1], c.ts.shadow_col[2], c.ts.shadow_col[3]};
    ts["stroke_enabled"] = c.ts.stroke_enabled;
    ts["stroke_w"]       = c.ts.stroke_w;
    ts["stroke_col"]     = {c.ts.stroke_col[0], c.ts.stroke_col[1], c.ts.stroke_col[2], c.ts.stroke_col[3]};
    ts["glow_enabled"]   = c.ts.glow_enabled;
    ts["glow_r"]         = c.ts.glow_r;
    ts["glow_col"]       = {c.ts.glow_col[0], c.ts.glow_col[1], c.ts.glow_col[2], c.ts.glow_col[3]};
    ts["bg_enabled"]     = c.ts.bg_enabled;
    ts["bg_col"]         = {c.ts.bg_col[0], c.ts.bg_col[1], c.ts.bg_col[2], c.ts.bg_col[3]};
    ts["bg_pad_x"]       = c.ts.bg_pad_x;
    ts["bg_pad_y"]       = c.ts.bg_pad_y;
    ts["bg_corner"]      = c.ts.bg_corner;
    j["text_style"]      = ts;

    // Callout overlay fields
    j["callout_style"] = c.callout_style;
    j["callout_arrow"] = c.callout_arrow;
    j["arrow_tx"]      = c.arrow_tx;
    j["arrow_ty"]      = c.arrow_ty;

    if (!c.source_id.empty())
        j["source"] = c.source_id;

    if (!c.runtime_fx_id.empty()) {
        j["runtime_fx_id"]     = c.runtime_fx_id;
        j["runtime_fx_amount"] = c.runtime_fx_amount;
    }
    if (c.clip_type == ClipType::BodyFX) {
        const BodyFXInfo* info = body_fx_find_info(c.body_fx_type);
        j["body_fx_type"]          = info ? info->name : "None";
        j["body_fx_amount"]        = c.body_fx_amount;
        j["body_fx_params"]        = {c.body_fx_params[0], c.body_fx_params[1],
                                      c.body_fx_params[2], c.body_fx_params[3]};
    }
    if (c.grade_brightness != 0.f || c.grade_contrast != 1.f ||
        c.grade_saturation != 1.f || c.grade_hue      != 0.f) {
        j["grade_brightness"] = c.grade_brightness;
        j["grade_contrast"]   = c.grade_contrast;
        j["grade_saturation"] = c.grade_saturation;
        j["grade_hue"]        = c.grade_hue;
    }
    return j;
}

static json state_to_json_slim(const AppState& state) {
    json j;
    j["duration"]         = state.duration;
    j["fps"]              = state.fps;
    j["bpm"]              = state.beat_bpm;
    j["audio_path"]       = state.audio_path;
    j["project_path"]     = state.project_path;
    j["transcript_ready"] = !state.words_json_path.empty() &&
                            (bool)std::ifstream(state.words_json_path);
    j["playhead"]         = state.playhead;
    json tracks_arr = json::array();
    for (int ti = 0; ti < (int)state.tracks.size(); ++ti) {
        const Track& t = state.tracks[ti];
        json tj;
        tj["index"]      = ti;
        tj["name"]       = t.name;
        tj["muted"]      = t.muted;
        tj["locked"]     = t.locked;
        tj["clip_count"] = (int)t.clips.size();
        tracks_arr.push_back(tj);
    }
    j["tracks"] = tracks_arr;
    json markers_arr = json::array();
    for (int mi = 0; mi < (int)state.markers.size(); ++mi) {
        const auto& m = state.markers[mi];
        char hex[12];
        snprintf(hex, sizeof(hex), "#%06X", m.color & 0x00FFFFFFu);
        json mj;
        mj["index"] = mi; mj["time"] = m.time;
        mj["label"] = m.label; mj["color"] = hex;
        markers_arr.push_back(mj);
    }
    j["markers"] = markers_arr;
    // Project bin — paths available to the project (not necessarily on the
    // timeline). New media goes here on multi-file drops or via add_to_bin.
    // Use add_clip with text=<path> to actually place a bin item.
    json bin_arr = json::array();
    for (auto& p : state.bin) bin_arr.push_back(p);
    j["bin"] = bin_arr;
    return j;
}

static json state_to_json(const AppState& state) {
    json j;
    j["duration"]        = state.duration;
    j["fps"]             = state.fps;
    j["bpm"]             = state.beat_bpm;
    j["audio_path"]      = state.audio_path;
    j["project_path"]    = state.project_path;
    j["transcript_ready"] = !state.words_json_path.empty() &&
                            (bool)std::ifstream(state.words_json_path);
    j["playhead"]        = state.playhead;

    // Beats (truncated to 3 decimal places)
    json beats_arr = json::array();
    for (float b : state.beats) {
        float rounded = (float)((int)(b * 1000 + 0.5f)) / 1000.f;
        beats_arr.push_back(rounded);
    }
    j["beats"] = beats_arr;

    json tracks_arr = json::array();
    for (int ti = 0; ti < (int)state.tracks.size(); ++ti) {
        const Track& t = state.tracks[ti];
        json tj;
        tj["index"]  = ti;
        tj["name"]   = t.name;
        tj["muted"]  = t.muted;
        tj["locked"] = t.locked;
        json clips_arr = json::array();
        for (int ci = 0; ci < (int)t.clips.size(); ++ci)
            clips_arr.push_back(clip_to_json(ci, t.clips[ci]));
        tj["clips"] = clips_arr;
        tracks_arr.push_back(tj);
    }
    j["tracks"] = tracks_arr;

    // Chapter markers
    json markers_arr = json::array();
    for (int mi = 0; mi < (int)state.markers.size(); ++mi) {
        const auto& m = state.markers[mi];
        char hex[12];
        snprintf(hex, sizeof(hex), "#%06X", m.color & 0x00FFFFFFu);
        json mj;
        mj["index"] = mi;
        mj["time"]  = m.time;
        mj["label"] = m.label;
        mj["color"] = hex;
        markers_arr.push_back(mj);
    }
    j["markers"] = markers_arr;

    // Project bin
    json bin_arr = json::array();
    for (auto& p : state.bin) bin_arr.push_back(p);
    j["bin"] = bin_arr;

    return j;
}

// ── ClipType parsing ──────────────────────────────────────────────────────────

static ClipType parse_clip_type(const std::string& s) {
    if (s == "audio")      return ClipType::Audio;
    if (s == "video")      return ClipType::Video;
    if (s == "text")       return ClipType::Text;
    if (s == "lyrics")     return ClipType::Lyrics;
    if (s == "subtitle")   return ClipType::Subtitle;
    if (s == "effect")     return ClipType::Effect;
    if (s == "background") return ClipType::Background;
    if (s == "body_fx")   return ClipType::BodyFX;
    return ClipType::Text;
}

// ── Bounds checking helpers ───────────────────────────────────────────────────

static bool check_track(const AppState& state, int ti, std::string& err) {
    if (ti < 0 || ti >= (int)state.tracks.size()) { err = "track index out of range"; return false; }
    return true;
}
static bool check_clip(const AppState& state, int ti, int ci, std::string& err) {
    if (!check_track(state, ti, err)) return false;
    if (ci < 0 || ci >= (int)state.tracks[ti].clips.size()) { err = "clip index out of range"; return false; }
    return true;
}
// Resolve track by name (track_name param) or integer index (track param).
static int track_by_name_or_index(const AppState& state, const json& params) {
    if (params.contains("track_name") && params["track_name"].is_string()) {
        const std::string name = params["track_name"].get<std::string>();
        for (int i = 0; i < (int)state.tracks.size(); ++i)
            if (state.tracks[i].name == name) return i;
        return -1;
    }
    return params.value("track", -1);
}

// ── Command dispatch ──────────────────────────────────────────────────────────

static json dispatch(AppState& state, const std::string& method, const json& params, std::string& err,
                     int client_fd = -1, const std::string& req_id = "") {
    // ── Log to agent activity panel ───────────────────────────────────────────
    {
        std::string detail;
        auto try_path = [&](const char* key) {
            if (detail.empty() && params.contains(key)) {
                std::string p = params[key].get<std::string>();
                auto pos = p.find_last_of("/\\");
                detail = (pos == std::string::npos) ? p : p.substr(pos + 1);
            }
        };
        try_path("src"); try_path("path"); try_path("media_path");
        if (detail.empty() && params.contains("query")) {
            detail = params["query"].get<std::string>();
            if (detail.size() > 28) detail = detail.substr(0, 25) + "...";
        }
        if (detail.empty() && params.contains("text")) {
            detail = params["text"].get<std::string>();
            if (detail.size() > 28) detail = detail.substr(0, 25) + "...";
        }
        state.agent_log.push_front({method, detail});
        if (state.agent_log.size() > 6) state.agent_log.pop_back();
    }

    // ── Read-only: no batch required ─────────────────────────────────────────
    if (method == "get_project") {
        bool verbose = params.value("verbose", false);
        return verbose ? state_to_json(state) : state_to_json_slim(state);
    }

    if (method == "get_clips") {
        int ti = track_by_name_or_index(state, params);
        if (!check_track(state, ti, err)) return {};
        json arr = json::array();
        for (int ci = 0; ci < (int)state.tracks[ti].clips.size(); ++ci)
            arr.push_back(clip_to_json_slim(ci, state.tracks[ti].clips[ci]));
        return arr;
    }

    if (method == "get_all_clips") {
        json arr = json::array();
        for (int ti = 0; ti < (int)state.tracks.size(); ++ti) {
            const Track& t = state.tracks[ti];
            json tj;
            tj["index"]  = ti;
            tj["name"]   = t.name;
            json clips = json::array();
            for (int ci = 0; ci < (int)t.clips.size(); ++ci)
                clips.push_back(clip_to_json_slim(ci, t.clips[ci]));
            tj["clips"] = clips;
            arr.push_back(tj);
        }
        return arr;
    }

    if (method == "get_export_status") {
        json r;
        r["running"]      = state.render.running;
        r["progress"]     = state.render.progress;
        r["frame"]        = state.render.frame;
        r["total_frames"] = state.render.total_frames;
        r["eta_secs"]     = state.render.eta_secs;
        r["stage"]        = state.render.stage;
        r["output"]       = state.out_mp4;
        return r;
    }

    if (method == "cancel_export") {
        render_cancel();
        return json::object();
    }

    if (method == "cancel_search") {
        transcribe_cancel();
        return json::object();
    }

    if (method == "trigger_export") {
        if (state.render.running) { err = "export already running"; return {}; }

        // Resolve output path
        if (params.contains("output_path") && !params["output_path"].get<std::string>().empty()) {
            state.export_out_path = params["output_path"].get<std::string>();
        } else if (!state.out_mp4.empty()) {
            state.export_out_path = state.out_mp4;
        } else {
            // Default: {project_dir}/{stem}.mp4 or ~/Videos/pop_maker_export.mp4
            std::string base;
            if (!state.project_path.empty()) {
                size_t sep  = state.project_path.rfind('/');
                std::string dir  = (sep != std::string::npos) ? state.project_path.substr(0, sep) : ".";
                std::string name = (sep != std::string::npos) ? state.project_path.substr(sep + 1) : state.project_path;
                size_t dot  = name.rfind('.');
                base = dir + "/" + (dot != std::string::npos ? name.substr(0, dot) : name);
            } else {
                const char* h = std::getenv("HOME");
                base = std::string(h ? h : ".") + "/Videos/pop_maker_export";
            }
            state.export_out_path = base + ".mp4";
        }

        // Optional render settings
        if (params.contains("crf"))    state.render_settings.crf          = params["crf"].get<int>();
        if (params.contains("preset")) state.render_settings.preset       = params["preset"].get<std::string>();
        if (params.contains("gif"))    state.render_settings.gif_export   = params["gif"].get<bool>();

        state.export_request = true;

        if (client_fd < 0) { json r; r["output"] = state.export_out_path; return r; }
        fd_mark_busy(client_fd);
        std::string out_path = state.export_out_path;
        std::thread([client_fd, req_id, out_path, &state]() {
            // Wait for GL thread to pick up the request and start rendering
            for (int i = 0; i < 100 && !state.render.running; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            while (state.render.running) {
                send_progress(client_fd, req_id, state.render.progress, state.render.stage);
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            json r;
            const std::string& stage = state.render.stage;
            bool ok = !stage.empty() &&
                      stage.rfind("Error", 0) == std::string::npos &&
                      stage != "Building\xe2\x80\xa6";   // UTF-8 ellipsis
            r["done"]    = true;
            r["output"]  = out_path;
            r["stage"]   = stage;
            r["success"] = ok;
            send_ok_id(client_fd, req_id, r);
            agent_done();
            fd_mark_free(client_fd);
        }).detach();
        json sentinel; sentinel["__async"] = true; return sentinel;
    }

    if (method == "get_beats") {
        json j;
        j["bpm"] = state.beat_bpm;
        json ba = json::array();
        for (float b : state.beats) ba.push_back(b);
        j["beats"] = ba;
        return j;
    }

    if (method == "get_pipeline_status") {
        json j;
        auto stage_str = [](PipelineStage s) -> std::string {
            switch (s) {
                case PipelineStage::Idle:       return "idle";
                case PipelineStage::Extract:    return "extract";
                case PipelineStage::Transcribe: return "transcribe";
                case PipelineStage::Align:      return "align";
                case PipelineStage::Done:       return "done";
                case PipelineStage::Error:      return "error";
                default:                        return "unknown";
            }
        };
        j["stage"]    = stage_str(state.pipeline.stage);
        j["progress"] = state.pipeline.progress;
        j["message"]  = state.pipeline.message;
        j["error"]    = state.pipeline.error;
        // When the pipeline lands on done, point callers at the managed lyrics
        // path. Suppress the hint if a Lyrics track already exists for the
        // current audio so re-polls during further edits aren't noisy.
        if (state.pipeline.stage == PipelineStage::Done && !state.audio_path.empty()) {
            bool lyrics_present = false;
            for (auto& t : state.tracks) {
                for (auto& c : t.clips)
                    if (c.clip_type == ClipType::Lyrics && c.source_id == state.audio_path)
                        { lyrics_present = true; break; }
                if (lyrics_present) break;
            }
            if (!lyrics_present)
                j["next_action_hint"] = "Transcript is ready. To lay lyric clips, call generate_typography(preset='flash'|'apple'|'spotify'|'karaoke'|'headline'|...). For raw word access without bricks, call get_transcript.";
        }
        return j;
    }

    if (method == "analyze_audio") {
        std::string path = params.value("path", "");
        if (path.empty()) { err = "path required"; return {}; }
        if (s_audio_analysis.running.load()) { err = "analysis already running"; return {}; }
        s_audio_analysis.running.store(true);
        s_audio_analysis.done.store(false);
        if (client_fd >= 0) {
            fd_mark_busy(client_fd);
            std::thread([path, client_fd, req_id]() {
                s_audio_analysis.result = beat_detect(path);
                s_audio_analysis.done.store(true);
                s_audio_analysis.running.store(false);
                auto& res = s_audio_analysis.result;
                json r;
                if (res.ok) {
                    r["status"]   = "done";
                    r["bpm"]      = res.bpm;
                    r["duration"] = res.duration;
                    r["beats"]    = res.beats;
                    r["rms"]      = res.rms;
                } else {
                    r["status"]  = "error";
                    r["message"] = "beat detection failed";
                }
                send_ok_id(client_fd, req_id, r);
                agent_done();
                fd_mark_free(client_fd);
            }).detach();
            json sentinel; sentinel["__async"] = true; return sentinel;
        }
        std::thread([path]() {
            s_audio_analysis.result = beat_detect(path);
            s_audio_analysis.done.store(true);
            s_audio_analysis.running.store(false);
        }).detach();
        json r; r["status"] = "started"; return r;
    }

    if (method == "get_audio_analysis") {
        if (s_audio_analysis.running.load()) { json r; r["status"] = "running"; return r; }
        if (!s_audio_analysis.done.load())   { json r; r["status"] = "idle"; return r; }
        auto& res = s_audio_analysis.result;
        if (!res.ok) { json r; r["status"] = "error"; r["message"] = "beat detection failed"; return r; }
        json r;
        r["status"]   = "done";
        r["bpm"]      = res.bpm;
        r["duration"] = res.duration;
        r["beats"]    = res.beats;
        r["rms"]      = res.rms;
        return r;
    }

    if (method == "describe_video") {
        std::string path = params.value("path", "");
        if (path.empty()) { err = "path required"; return {}; }
        if (s_scene_analysis.running.load()) { err = "scene analysis already running"; return {}; }
        s_scene_analysis.running.store(true);
        s_scene_analysis.done.store(false);
        s_scene_analysis.error.clear();
        s_scene_analysis.sidecar_path.clear();
        std::thread([path]() {
            bool capped = false;
            std::vector<KeyFrame> frames = extract_keyframes(path, 60, &capped);
            if (frames.empty()) {
                s_scene_analysis.error = "keyframe extraction failed or no scene changes detected";
                s_scene_analysis.done.store(true);
                s_scene_analysis.running.store(false);
                return;
            }
            // Caption each frame
            json sidecar;
            sidecar["source"] = path;
            sidecar["model"]  = "moondream2";
            sidecar["capped"] = capped;
            json frames_arr = json::array();
            for (auto& kf : frames) {
                SceneResult res = caption_frame(kf.jpeg_path);
                json entry;
                entry["timestamp"]   = kf.timestamp;
                entry["description"] = res.ok ? res.description : "";
                frames_arr.push_back(entry);
            }
            sidecar["frames"] = frames_arr;
            std::string sidecar_path = path + ".pms_scene.json";
            {
                std::ofstream f(sidecar_path);
                f << sidecar.dump(2);
            }
            s_scene_analysis.sidecar_path = sidecar_path;
            s_scene_analysis.done.store(true);
            s_scene_analysis.running.store(false);
        }).detach();
        json r; r["status"] = "started";
        return r;
    }

    if (method == "get_video_description") {
        if (s_scene_analysis.running.load()) { json r; r["status"] = "running"; return r; }
        if (!s_scene_analysis.done.load())   { json r; r["status"] = "idle"; return r; }
        if (!s_scene_analysis.error.empty()) {
            json r; r["status"] = "error"; r["message"] = s_scene_analysis.error; return r;
        }
        // Read sidecar JSON and return it
        std::string sp = s_scene_analysis.sidecar_path;
        if (sp.empty()) { json r; r["status"] = "error"; r["message"] = "no sidecar path"; return r; }
        try {
            std::ifstream f(sp);
            json sc; f >> sc;
            json r;
            r["status"]  = "done";
            r["sidecar"] = sp;
            r["frames"]  = sc.value("frames", json::array());
            r["capped"]  = sc.value("capped", false);
            return r;
        } catch (...) {
            json r; r["status"] = "error"; r["message"] = "failed to read sidecar"; return r;
        }
    }

    if (method == "get_vision_model_status") {
        json r;
        if (vision_models_ready()) {
            r["status"] = "ready";
        } else if (vision_download_running()) {
            r["status"]   = "downloading";
            r["progress"] = vision_download_progress();
            r["message"]  = vision_download_message();
        } else if (!vision_download_error().empty()) {
            r["status"] = "error";
            r["error"]  = vision_download_error();
        } else {
            r["status"] = "idle";
        }
        return r;
    }

    if (method == "download_vision_model") {
        if (vision_models_ready()) { json r; r["status"] = "ready"; return r; }
        vision_download_start();
        json r; r["status"] = "started";
        return r;
    }

    if (method == "extract_clip_segment") {
        std::string src = params.value("src", "");
        std::string dst = params.value("dst", "");
        double start    = params.value("start", 0.0);
        double end      = params.value("end", 0.0);
        if (src.empty()) { err = "src required"; return {}; }
        if (dst.empty()) { err = "dst required"; return {}; }
        if (end <= start) { err = "end must be greater than start"; return {}; }
        std::string extract_err = video_extract_segment(src, start, end, dst);
        if (!extract_err.empty()) { err = extract_err; return {}; }
        json r;
        r["dst"]      = dst;
        r["duration"] = end - start;
        return r;
    }

    if (method == "get_search_status") {
        SearchStatus s = transcribe_search_status();
        json r;
        r["running"]     = s.running;
        r["progress"]    = s.progress;
        r["current_sec"] = s.current_sec;
        r["total_sec"]   = s.total_sec;
        r["message"]     = s.message;
        r["found"]       = s.found;
        r["start"]       = s.start;
        r["end"]         = s.end;
        r["excerpt"]     = s.excerpt;
        r["error"]       = s.error;
        // Top-N closest fuzzy matches when found=false.  Empty when found=true
        // or the accumulated transcript was too sparse to score candidates.
        json cands = json::array();
        for (const auto& c : s.candidates) {
            json cc;
            cc["start"]   = c.start;
            cc["end"]     = c.end;
            cc["score"]   = c.score;
            cc["excerpt"] = c.excerpt;
            cands.push_back(cc);
        }
        r["candidates"] = cands;
        return r;
    }

    if (method == "search_transcript") {
        std::string path = params.value("path", "");
        float buffer_sec = params.value("buffer_sec", 60.f);
        if (path.empty()) { err = "path required"; return {}; }
        auto qj = params.value("query_words", json::array());
        std::vector<std::string> qw;
        for (auto& w : qj) if (w.is_string()) qw.push_back(w.get<std::string>());
        if (qw.empty()) { err = "query_words required"; return {}; }
        if (transcribe_search_running()) { err = "search already running"; return {}; }
        transcribe_search_start(path, qw, buffer_sec);
        json res; res["status"] = "started"; return res;
    }

    if (method == "set_audio_path") {
        std::string path = params.value("path", "");
        if (path.empty()) { err = "path required"; return {}; }
        state.audio_path = path;
        return json::object();
    }

    if (method == "get_media_info") {
        std::string path = params.value("path", "");
        if (path.empty()) { err = "path required"; return {}; }
        MediaFileInfo mi = video_probe_file(path);
        if (!mi.error.empty()) { err = mi.error; return {}; }
        json r;
        r["duration"]    = mi.duration;
        r["width"]       = mi.width;
        r["height"]      = mi.height;
        r["fps"]         = mi.fps;
        r["has_video"]   = mi.has_video;
        r["has_audio"]   = mi.has_audio;
        r["video_codec"] = mi.video_codec;
        r["audio_codec"] = mi.audio_codec;
        r["sample_rate"] = mi.sample_rate;
        r["channels"]    = mi.channels;
        return r;
    }

    // Canonical cache path: <dir>/<stem>/<stem>_words.json (matches pipeline writer)
    auto words_cache_path_for = [](const std::string& audio_path) -> std::filesystem::path {
        if (audio_path.empty()) return {};
        std::filesystem::path p(audio_path);
        return p.parent_path() / p.stem() / (p.stem().string() + "_words.json");
    };
    // Search cache path: <dir>/<stem>/<stem>_words_search.json (windowed
    // find_and_add_clip writes here).  Used as a fallback when the
    // canonical (full-pipeline) cache hasn't been produced yet.
    auto search_words_path_for = [](const std::string& audio_path) -> std::filesystem::path {
        if (audio_path.empty()) return {};
        std::filesystem::path p(audio_path);
        return p.parent_path() / p.stem() / (p.stem().string() + "_words_search.json");
    };

    if (method == "get_transcript") {
        json r;
        std::string ext_path = params.value("path", std::string());
        std::filesystem::path src;
        std::string provenance = "canonical";
        if (!ext_path.empty()) {
            src = words_cache_path_for(ext_path);
            if (!std::filesystem::exists(src)) {
                auto fallback = search_words_path_for(ext_path);
                if (std::filesystem::exists(fallback)) {
                    src        = fallback;
                    provenance = "search";
                }
            }
        }
        else if (!state.words_json_path.empty()) src = state.words_json_path;
        else { r["status"] = "idle"; return r; }

        if (!std::filesystem::exists(src)) { r["status"] = "idle"; return r; }
        std::ifstream f(src.string());
        if (!f.is_open()) { r["status"] = "idle"; return r; }
        try {
            json words = json::parse(f);
            r["status"]     = "ready";
            r["words"]      = words;
            r["path"]       = src.string();
            r["provenance"] = provenance;  // "canonical" or "search"
        } catch (...) {
            r["status"]  = "error";
            r["message"] = "failed to parse words JSON";
        }
        return r;
    }

    if (method == "set_transcript") {
        if (!params.contains("words") || !params["words"].is_array()) {
            err = "words array required: [{word, start, end}, ...]"; return {};
        }
        if (state.audio_path.empty()) {
            err = "no audio source on project — add an audio clip first so the transcript has a target"; return {};
        }
        auto target = words_cache_path_for(state.audio_path);
        std::error_code ec;
        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec) { err = "failed to create cache dir: " + ec.message(); return {}; }

        json out = json::array();
        int skipped = 0;
        for (auto& w : params["words"]) {
            if (!w.contains("word") || !w.contains("start") || !w.contains("end")) {
                skipped++; continue;
            }
            json entry;
            entry["word"]  = w["word"].get<std::string>();
            entry["start"] = w["start"].get<float>();
            entry["end"]   = w["end"].get<float>();
            out.push_back(entry);
        }

        std::ofstream f(target.string());
        if (!f) { err = "failed to write " + target.string(); return {}; }
        f << out.dump(2);
        f.close();

        state.words_json_path = target.string();
        load_words_cache(state);

        json r;
        r["path"]    = target.string();
        r["count"]   = (int)state.words_cache.size();
        r["skipped"] = skipped;
        r["next_action_hint"] = "Custom transcript installed at " + target.string()
            + ". Call generate_typography(preset='flash'|'apple'|'spotify'|...) to lay lyric clips against it.";
        return r;
    }

    if (method == "shift_transcript") {
        if (state.audio_path.empty()) {
            err = "no audio source on project — add an audio clip first"; return {};
        }
        float offset      = params.value("offset", 0.f);
        float range_start = params.value("start", -std::numeric_limits<float>::infinity());
        float range_end   = params.value("end",    std::numeric_limits<float>::infinity());
        std::string sp    = params.value("source_path", std::string());

        std::filesystem::path src_cache;
        if (!sp.empty())                          src_cache = words_cache_path_for(sp);
        else if (!state.words_json_path.empty())  src_cache = state.words_json_path;

        if (src_cache.empty() || !std::filesystem::exists(src_cache)) {
            err = "source transcript not found"
                + std::string(src_cache.empty() ? "" : " at " + src_cache.string())
                + " — run trigger_pipeline first or pass source_path of a previously transcribed file";
            return {};
        }

        std::ifstream f(src_cache.string());
        json src_words;
        try { src_words = json::parse(f); }
        catch (...) { err = "failed to parse source transcript at " + src_cache.string(); return {}; }
        if (!src_words.is_array()) { err = "source transcript is not a JSON array"; return {}; }

        json out = json::array();
        for (auto& w : src_words) {
            if (!w.contains("start") || !w.contains("end") || !w.contains("word")) continue;
            float s = w["start"].get<float>();
            float e = w["end"].get<float>();
            // Range is in SOURCE-file time (pre-shift); keep words that overlap the range
            if (e < range_start || s > range_end) continue;
            json entry;
            entry["word"]  = w["word"].get<std::string>();
            entry["start"] = s + offset;
            entry["end"]   = e + offset;
            out.push_back(entry);
        }

        auto target = words_cache_path_for(state.audio_path);
        std::error_code ec;
        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec) { err = "failed to create cache dir: " + ec.message(); return {}; }

        std::ofstream wf(target.string());
        if (!wf) { err = "failed to write " + target.string(); return {}; }
        wf << out.dump(2);
        wf.close();

        state.words_json_path = target.string();
        load_words_cache(state);

        json r;
        r["path"]   = target.string();
        r["count"]  = (int)state.words_cache.size();
        r["source"] = src_cache.string();
        r["offset"] = offset;
        r["next_action_hint"] = "Transcript shifted/clipped and installed. Call generate_typography(preset='flash'|...) to lay lyric clips.";
        return r;
    }

    if (method == "save_project") {
        std::string path = params.value("path", state.project_path);
        if (path.empty()) { err = "no project path — provide 'path' param or save once from UI"; return {}; }
        if (!project_save(state, path)) { err = "project_save failed"; return {}; }
        json r; r["path"] = path;
        return r;
    }

    if (method == "seek") {
        float t = params.value("time", 0.f);
        state.playhead = t;
        return json::object();
    }

    if (method == "play") {
        if (!state.playing) {
            state.playing = true;
            state.play_start_wall = std::chrono::steady_clock::now();
            state.play_start_pos  = state.playhead;
        }
        return json::object();
    }

    if (method == "pause") {
        state.playing = false;
        return json::object();
    }

    if (method == "validate_glsl") {
        std::string glsl = params.value("glsl", "");
        std::vector<RuntimeFXParam> ps;
        if (params.contains("params") && params["params"].is_array()) {
            for (auto& p : params["params"]) {
                RuntimeFXParam fp;
                fp.name        = p.value("name",    "p");
                fp.label       = p.value("label",   fp.name);
                fp.min_val     = p.value("min",     0.f);
                fp.max_val     = p.value("max",     1.f);
                fp.default_val = p.value("default", 0.f);
                ps.push_back(fp);
            }
        }
        std::string compile_err = runtime_fx_validate(glsl, ps);
        json r;
        r["ok"] = compile_err.empty();
        if (!compile_err.empty()) r["error"] = compile_err;
        return r;
    }

    // ── Batch control ─────────────────────────────────────────────────────────
    if (method == "begin_batch") {
        if (g_in_batch) { err = "already in a batch"; return {}; }
        g_in_batch    = true;
        g_batch_label = params.value("label", "MCP edit");
        g_batch_state = &state;
        state.agent_active = true;
        state.agent_msg    = g_batch_label;
        return json::object();
    }

    if (method == "end_batch") {
        if (!g_in_batch) { err = "not in a batch"; return {}; }
        history_push(state, g_batch_label);
        g_in_batch         = false;
        g_auto_batched     = false;
        g_batch_label.clear();
        g_batch_state      = nullptr;
        state.agent_active = false;
        state.agent_msg.clear();
        json r;
        r["duration"]         = state.duration;
        r["project_path"]     = state.project_path;
        r["transcript_ready"] = !state.words_json_path.empty() &&
                                (bool)std::ifstream(state.words_json_path);
        return r;
    }

    // ── Snapshot (GL-thread flag — fulfilled by draw_preview) ─────────────────
    // Lightweight UI presence for server-side jobs (e.g. the MCP server's
    // activity scan): refreshes the agent pill with a progress message each
    // call; {"done": true} clears it. No batching, no history — the canvas
    // auto-timeout still reaps a stale flag if the job dies mid-run.
    if (method == "agent_status") {
        if (params.value("done", false)) {
            state.agent_active = false;
            state.agent_msg.clear();
        } else {
            state.agent_active = true;
            state.agent_msg    = params.value("msg", "working…");
        }
        return json::object();
    }

    if (method == "take_snapshot") {
        if (params.contains("time"))
            state.playhead = params["time"].get<float>();
        state.snapshot_done      = false;
        state.snapshot_done_path.clear();
        state.snapshot_done_err.clear();
        state.snapshot_source_canvas = (params.value("source", "render") == "canvas");
        state.snapshot_request   = true;
        return json::object();
    }

    if (method == "get_snapshot_status") {
        json r;
        r["done"] = state.snapshot_done;
        if (state.snapshot_done) {
            if (state.snapshot_done_err.empty())
                r["path"] = state.snapshot_done_path;
            else
                r["error"] = state.snapshot_done_err;
        }
        return r;
    }

    // ── Undo / redo ───────────────────────────────────────────────────────────
    if (method == "undo") {
        if (!history_can_undo()) { err = "nothing to undo"; return {}; }
        history_undo(state);
        return state_to_json_slim(state);
    }

    if (method == "redo") {
        if (!history_can_redo()) { err = "nothing to redo"; return {}; }
        history_redo(state);
        return state_to_json_slim(state);
    }

    // ── Background removal / BodyFX mask processing ───────────────────────────
    if (method == "start_body_fx_process") {
        int ti = params.value("track", -1), ci = params.value("clip", -1);
        if (!check_clip(state, ti, ci, err)) return {};
        const Clip& brick = state.tracks[ti].clips[ci];
        if (brick.clip_type != ClipType::BodyFX) { err = "clip is not a body_fx brick"; return {}; }
        int vci = -1;
        for (int vi = 0; vi < (int)state.tracks[ti].clips.size(); ++vi) {
            const Clip& vc = state.tracks[ti].clips[vi];
            if (vc.clip_type != ClipType::Video) continue;
            if (vc.end <= brick.start || vc.start >= brick.end) continue;
            vci = vi;
            break;
        }
        if (vci < 0) { err = "no video clip found on the same track overlapping the brick"; return {}; }
        // Pre-check proxy: body_fx masks need the same proxy frames bg_remove
        // does. Fail synchronously when the proxy isn't ready instead of
        // parking the clip in WaitingForProxy and spinning the wait loop.
        const Clip& vc_pre = state.tracks[ti].clips[vci];
        if (vc_pre.text.empty() || !proxy_is_ready(vc_pre.text)) {
            err = "video proxy not ready yet — body_fx masks require the MJPEG "
                  "proxy on disk; wait for it to finish generating, then retry";
            return {};
        }
        Clip& vc_mut = state.tracks[ti].clips[vci];
        if (vc_mut.bg_remove_status == BgRemoveStatus::Idle ||
            vc_mut.bg_remove_status == BgRemoveStatus::Error)
            bg_remove_start(state, ti, vci);
        if (client_fd < 0) {
            json r; r["video_track"] = ti; r["video_clip"] = vci; return r;
        }
        fd_mark_busy(client_fd);
        Clip* clip_ptr = &state.tracks[ti].clips[vci];
        int vti = ti;
        std::thread([client_fd, req_id, clip_ptr, vti, vci]() {
            while (true) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                float           prog = clip_ptr->bg_remove_progress;
                BgRemoveStatus  st   = clip_ptr->bg_remove_status;
                send_progress(client_fd, req_id, prog, "");
                if (st == BgRemoveStatus::Ready || st == BgRemoveStatus::Error) {
                    json r;
                    r["status"]      = (st == BgRemoveStatus::Ready) ? "ready" : "error";
                    r["progress"]    = prog;
                    r["error"]       = clip_ptr->bg_remove_error;
                    r["video_track"] = vti;
                    r["video_clip"]  = vci;
                    send_ok_id(client_fd, req_id, r);
                    agent_done();
                    fd_mark_free(client_fd);
                    return;
                }
            }
        }).detach();
        json sentinel; sentinel["__async"] = true; return sentinel;
    }

    if (method == "start_bg_remove") {
        int ti = params.value("track", -1), ci = params.value("clip", -1);
        if (!check_clip(state, ti, ci, err)) return {};
        const Clip& cl_pre = state.tracks[ti].clips[ci];
        if (cl_pre.clip_type != ClipType::Video || cl_pre.text.empty()) {
            err = "clip is not a video clip"; return {};
        }
        // Refuse fast when the MJPEG proxy isn't on disk yet — the bg_remove
        // mask pipeline reads frames from the proxy, so without it we'd
        // silently park the clip in WaitingForProxy and the caller's wait loop
        // would spin until proxy generation eventually finishes. Make it an
        // explicit, actionable error so the agent can poll get_pipeline_status
        // / proxy progress and retry.
        if (!proxy_is_ready(cl_pre.text)) {
            err = "video proxy not ready yet — bg_remove requires the MJPEG "
                  "proxy on disk; wait for it to finish generating, then retry";
            return {};
        }
        bg_remove_start(state, ti, ci);
        if (client_fd < 0) { return json::object(); }
        fd_mark_busy(client_fd);
        Clip* clip_ptr = &state.tracks[ti].clips[ci];
        std::thread([client_fd, req_id, clip_ptr]() {
            while (true) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                float           prog = clip_ptr->bg_remove_progress;
                BgRemoveStatus  st   = clip_ptr->bg_remove_status;
                send_progress(client_fd, req_id, prog, "");
                if (st == BgRemoveStatus::Ready || st == BgRemoveStatus::Error) {
                    json r;
                    r["status"]   = (st == BgRemoveStatus::Ready) ? "ready" : "error";
                    r["progress"] = prog;
                    r["error"]    = clip_ptr->bg_remove_error;
                    send_ok_id(client_fd, req_id, r);
                    agent_done();
                    fd_mark_free(client_fd);
                    return;
                }
            }
        }).detach();
        json sentinel; sentinel["__async"] = true; return sentinel;
    }

    if (method == "get_bg_remove_status") {
        int ti = params.value("track", -1), ci = params.value("clip", -1);
        if (!check_clip(state, ti, ci, err)) return {};
        const Clip& cl = state.tracks[ti].clips[ci];
        json r;
        std::string st;
        switch (cl.bg_remove_status) {
            case BgRemoveStatus::Idle:             st = "idle";              break;
            case BgRemoveStatus::WaitingForProxy:  st = "waiting_for_proxy"; break;
            case BgRemoveStatus::Processing:       st = "processing";        break;
            case BgRemoveStatus::Ready:            st = "ready";             break;
            case BgRemoveStatus::Error:            st = "error";             break;
        }
        r["status"]   = st;
        r["progress"] = cl.bg_remove_progress;
        r["error"]    = cl.bg_remove_error;
        return r;
    }

    // ── Editing: auto-batch single mutations if not already in a batch ────────
    if (!g_in_batch) {
        g_in_batch     = true;
        g_auto_batched = true;
        g_batch_label  = method;
        g_batch_state  = &state;
    }

    if (method == "move_clip") {
        int ti = track_by_name_or_index(state, params), ci = params.value("clip", -1);
        float start = snap_to_frame(params.value("start", 0.f), state.fps);
        if (!check_clip(state, ti, ci, err)) return {};
        Clip& cl = state.tracks[ti].clips[ci];
        float dur = cl.end - cl.start;
        cl.start = start;
        cl.end   = start + dur;
        return json::object();
    }

    if (method == "trim_clip") {
        int ti = track_by_name_or_index(state, params), ci = params.value("clip", -1);
        if (!check_clip(state, ti, ci, err)) return {};
        Clip& cl = state.tracks[ti].clips[ci];
        float old_start = cl.start;
        if (params.contains("start")) {
            float ns = snap_to_frame(params["start"].get<float>(), state.fps);
            float delta = ns - old_start;
            if (delta > 0.f) cl.in_point += delta;
            cl.start = ns;
        }
        if (params.contains("end")) cl.end = snap_end_to_frame(params["end"].get<float>(), state.fps);
        return json::object();
    }

    if (method == "split_clip") {
        int ti = track_by_name_or_index(state, params), ci = params.value("clip", -1);
        float t = params.value("time", -1.f);
        if (!check_clip(state, ti, ci, err)) return {};
        Clip& cl = state.tracks[ti].clips[ci];
        if (t <= cl.start || t >= cl.end) { err = "split time outside clip range"; return {}; }

        Clip right = cl;
        right.in_point = cl.in_point + (t - cl.start);
        right.start = t;

        cl.end = t;
        state.tracks[ti].clips.insert(state.tracks[ti].clips.begin() + ci + 1, right);
        json r;
        r["left_clip"]  = ci;
        r["right_clip"] = ci + 1;
        return r;
    }

    if (method == "delete_clip") {
        int ti = track_by_name_or_index(state, params), ci = params.value("clip", -1);
        if (!check_clip(state, ti, ci, err)) return {};
        state.tracks[ti].clips.erase(state.tracks[ti].clips.begin() + ci);
        if (state.selected_track == ti && state.selected_clip == ci)
            state.selected_clip = -1;
        return json::object();
    }

    if (method == "add_clip") {
        int ti = track_by_name_or_index(state, params);
        if (!check_track(state, ti, err)) return {};
        if (state.tracks[ti].locked) { err = "track is locked"; return {}; }
        std::string type_s = params.value("type", "text");
        float start = snap_to_frame(params.value("start", 0.f), state.fps);
        float end   = snap_end_to_frame(params.value("end", start + 2.f), state.fps);
        std::string text = params.value("text", "");

        Clip cl;
        cl.clip_type = parse_clip_type(type_s);
        cl.start = start;
        cl.end   = end;
        cl.text  = text;
        if (cl.clip_type == ClipType::Video || cl.clip_type == ClipType::Audio) {
            cl.source_id = text;
            // Mirror into the bin so any media that ends up on the timeline is
            // also listed in the project — agents calling add_clip with a path
            // they discovered via search or filesystem don't need a separate
            // add_to_bin round-trip.
            if (!text.empty()) bin_add(state, text);
        }
        // Images are ClipType::Video but carry no audio — never let a PNG
        // become the project's audio source (it used to, and even kicked a
        // WhisperX transcription on the pixels below).
        bool is_image_clip = cl.clip_type == ClipType::Video && is_image_path(text);
        if ((cl.clip_type == ClipType::Video || cl.clip_type == ClipType::Audio) &&
            !is_image_clip && state.audio_path.empty())
            state.audio_path = state.vocals_path = text;
        state.tracks[ti].clips.push_back(cl);
        int new_ci = (int)state.tracks[ti].clips.size() - 1;

        // Auto-refresh audio_path when the project's current audio source is no
        // longer on the timeline. Agents adding a *different* audio file as the
        // sole audio clip otherwise hit "transcript already cached" because
        // audio_path stayed pinned to the previous source. Mirrors the UI flow
        // where dropping a new audio file makes that file the project audio.
        if (cl.clip_type == ClipType::Audio && !text.empty() && state.audio_path != text) {
            bool referenced = false;
            for (int tt = 0; tt < (int)state.tracks.size() && !referenced; ++tt) {
                for (int cc = 0; cc < (int)state.tracks[tt].clips.size() && !referenced; ++cc) {
                    if (tt == ti && cc == new_ci) continue;
                    auto& other = state.tracks[tt].clips[cc];
                    if ((other.clip_type == ClipType::Audio || other.clip_type == ClipType::Video)
                        && other.text == state.audio_path) referenced = true;
                }
            }
            if (!referenced) {
                state.audio_path = text;
                state.vocals_path.clear();
                state.words_json_path.clear();
                state.words_cache.clear();
            }
        }

        if (cl.clip_type == ClipType::Video) {
            proxy_start(text);
            state.proxy_scan_needed = true;
        }
        if (cl.clip_type == ClipType::Video && !is_image_clip && !state.audio_path.empty() &&
            state.words_json_path.empty() && !transcribe_running())
            kick_pipeline(state, state.audio_path, PipelineMode::TranscribeOnly);
        if (cl.clip_type == ClipType::BodyFX) {
            for (int vi = 0; vi < new_ci; ++vi) {
                Clip& vc = state.tracks[ti].clips[vi];
                if (vc.clip_type != ClipType::Video) continue;
                if (vc.end <= cl.start || vc.start >= cl.end) continue;
                // Select the video clip so its existing bg_remove progress bar shows
                state.selected_track = ti;
                state.selected_clip  = vi;
                if (vc.bg_remove_status != BgRemoveStatus::Ready)
                    bg_remove_start(state, ti, vi);
                break;
            }
        }
        json r; r["clip"] = new_ci;

        // Soft-warn when an agent hand-builds a text clip while a fresh
        // transcript is available and no managed Lyrics track exists yet.
        // The managed path (generate_typography) is almost always the right
        // call for lyrics/captions; type='text' should be reserved for one-off
        // labels or callouts. This is a hint only — the clip is still created.
        if (cl.clip_type == ClipType::Text && !state.audio_path.empty()) {
            bool transcript_ready = !state.words_json_path.empty()
                && (bool)std::ifstream(state.words_json_path);
            if (transcript_ready) {
                bool lyrics_present = false;
                for (auto& t : state.tracks) {
                    for (auto& cc : t.clips)
                        if (cc.clip_type == ClipType::Lyrics && cc.source_id == state.audio_path)
                            { lyrics_present = true; break; }
                    if (lyrics_present) break;
                }
                if (!lyrics_present)
                    r["warning"] = "A transcript is ready for this project's audio but no managed Lyrics track exists. For lyric videos / captions, prefer generate_typography(preset='flash'|'apple'|'spotify'|...) — it lays styled, idempotent lyric clips on a managed track. type='text' is intended for one-off labels (or use add_callout).";
            }
        }
        return r;
    }

    if (method == "add_to_bin") {
        if (!params.contains("path") || !params["path"].is_string()) {
            err = "path required"; return {};
        }
        std::string path = params["path"].get<std::string>();
        if (path.empty()) { err = "path required"; return {}; }
        bin_add(state, path);
        json r;
        r["path"]     = path;
        r["bin_size"] = (int)state.bin.size();
        return r;
    }

    if (method == "remove_from_bin") {
        if (!params.contains("path") || !params["path"].is_string()) {
            err = "path required"; return {};
        }
        std::string path = params["path"].get<std::string>();
        bin_remove(state, path);
        json r;
        r["path"]     = path;
        r["bin_size"] = (int)state.bin.size();
        return r;
    }

    if (method == "add_clip_sequence") {
        int ti = params.value("track", -1);
        if (!check_track(state, ti, err)) return {};
        if (state.tracks[ti].locked) { err = "track is locked"; return {}; }
        if (!params.contains("clips") || !params["clips"].is_array()) { err = "clips array required"; return {}; }
        json ids = json::array();
        for (auto& entry : params["clips"]) {
            std::string type_s = entry.value("type", "text");
            float start = snap_to_frame(entry.value("start", 0.f), state.fps);
            float end   = snap_end_to_frame(entry.value("end", start + 2.f), state.fps);
            std::string text = entry.value("text", "");
            Clip cl;
            cl.clip_type = parse_clip_type(type_s);
            cl.start = start; cl.end = end; cl.text = text;
            if (cl.clip_type == ClipType::Video || cl.clip_type == ClipType::Audio)
                cl.source_id = text;
            state.tracks[ti].clips.push_back(cl);
            if (cl.clip_type == ClipType::Video) {
                proxy_start(text);
                state.proxy_scan_needed = true;
            }
            ids.push_back((int)state.tracks[ti].clips.size() - 1);
        }
        json r; r["clips"] = ids;
        return r;
    }

    if (method == "set_clip_props") {
        if (!params.contains("ops") || !params["ops"].is_array()) { err = "ops array required"; return {}; }
        for (auto& op : params["ops"]) {
            int ti = op.value("track", -1), ci = op.value("clip", -1);
            std::string prop = op.value("prop", "");
            if (!check_clip(state, ti, ci, err)) return {};
            Clip& cl = state.tracks[ti].clips[ci];
            auto& val = op["value"];
            if      (prop == "volume")    { cl.volume    = jval_float(val); }
            else if (prop == "speed")     { cl.speed = fmaxf(0.25f, fminf(100.f, jval_float(val))); }
            else if (prop == "opacity")   { cl.opacity   = jval_float(val); }
            else if (prop == "muted")     { cl.muted     = jval_bool(val); }
            else if (prop == "in_point")  { cl.in_point  = jval_float(val); }
            else if (prop == "fade_in")   { cl.fade_in   = jval_float(val); }
            else if (prop == "fade_out")  { cl.fade_out  = jval_float(val); }
            else if (prop == "blend_mode"){ cl.blend_mode= jval_int(val); }
            else if (prop == "pos_x")     { cl.pos_x     = jval_float(val); }
            else if (prop == "pos_y")     { cl.pos_y     = jval_float(val); }
            else if (prop == "scale_x")   { cl.scale_x   = jval_float(val); }
            else if (prop == "scale_y")   { cl.scale_y   = jval_float(val); }
            else if (prop == "rotation")  { cl.rotation  = jval_float(val); }
            // Crop: clamp against the opposite side so a sliver stays visible
            else if (prop == "crop_l")    { cl.crop_l = fmaxf(0.f, fminf(jval_float(val), 0.95f - cl.crop_r)); }
            else if (prop == "crop_r")    { cl.crop_r = fmaxf(0.f, fminf(jval_float(val), 0.95f - cl.crop_l)); }
            else if (prop == "crop_t")    { cl.crop_t = fmaxf(0.f, fminf(jval_float(val), 0.95f - cl.crop_b)); }
            else if (prop == "crop_b")    { cl.crop_b = fmaxf(0.f, fminf(jval_float(val), 0.95f - cl.crop_t)); }
            else if (prop == "text")      { cl.text      = val.get<std::string>(); }
            else if (prop == "font_size") { cl.font_size = jval_float(val); }
            else if (prop == "sub_pos")   { cl.sub_pos   = jval_int(val); }
            else if (prop == "sub_pos_x") { cl.sub_pos_x = jval_float(val); }
            else if (prop == "sub_pos_y") { cl.sub_pos_y = jval_float(val); }
            else if (prop == "sub_anchor_h") { cl.sub_anchor_h = jval_int(val); }
            else if (prop == "sub_wrap_w"){ cl.sub_wrap_w= jval_float(val); }
            else if (prop == "karaoke")   { cl.karaoke   = jval_bool(val); }
            else if (prop == "clip_style"){ cl.clip_style= parse_anim_style(val.get<std::string>()); }
            else if (prop == "sub_color") {
                if (!val.is_array() || val.size() != 4) { err = "sub_color must be [r,g,b,a]"; return {}; }
                for (int i = 0; i < 4; ++i) cl.sub_color[i] = jval_float(val[i]);
                cl.sub_color_override = true;
            }
            else if (prop == "karaoke_highlight_color") {
                if (!val.is_array() || val.size() != 4) { err = "karaoke_highlight_color must be [r,g,b,a]"; return {}; }
                for (int i = 0; i < 4; ++i) cl.karaoke_highlight_color[i] = jval_float(val[i]);
            }
            else if (prop == "callout_style") { cl.callout_style = jval_int(val); }
            else if (prop == "callout_arrow") { cl.callout_arrow = jval_bool(val); }
            else if (prop == "arrow_tx")      { cl.arrow_tx      = jval_float(val); }
            else if (prop == "arrow_ty")      { cl.arrow_ty      = jval_float(val); }
            else if (prop == "body_fx_type") {
                std::string name = val.get<std::string>();
                int n = body_fx_info_count();
                const BodyFXInfo* infos = body_fx_info_list();
                bool found = false;
                for (int i = 0; i < n; ++i) {
                    if (infos[i].name == name) {
                        cl.body_fx_type = infos[i].type;
                        for (int pi = 0; pi < 4; ++pi)
                            cl.body_fx_params[pi] = pi < infos[i].n_params ? infos[i].params[pi].default_val : 0.5f;
                        found = true; break;
                    }
                }
                if (!found) { err = "unknown body_fx_type: " + name; return {}; }
            }
            else if (prop == "body_fx_amount")  { cl.body_fx_amount     = jval_float(val); }
            else if (prop == "body_fx_param_0") { cl.body_fx_params[0]  = jval_float(val); }
            else if (prop == "body_fx_param_1") { cl.body_fx_params[1]  = jval_float(val); }
            else if (prop == "body_fx_param_2") { cl.body_fx_params[2]  = jval_float(val); }
            else if (prop == "body_fx_param_3") { cl.body_fx_params[3]  = jval_float(val); }
            else if (prop == "grade_brightness") { cl.grade_brightness = jval_float(val); }
            else if (prop == "grade_contrast")   { cl.grade_contrast   = jval_float(val); }
            else if (prop == "grade_saturation") { cl.grade_saturation = jval_float(val); }
            else if (prop == "grade_hue")        { cl.grade_hue        = jval_float(val); }
            else { err = "unknown prop: " + prop; return {}; }
        }
        return json::object();
    }

    if (method == "set_clip_prop") {
        int ti = track_by_name_or_index(state, params), ci = params.value("clip", -1);
        std::string prop = params.value("prop", "");
        if (!check_clip(state, ti, ci, err)) return {};
        Clip& cl = state.tracks[ti].clips[ci];

        auto& val = params["value"];
        // ── A/V props ────────────────────────────────────────────────────────
        if      (prop == "volume")   { cl.volume     = jval_float(val); }
        else if (prop == "speed")    { cl.speed = fmaxf(0.25f, fminf(100.f, jval_float(val))); }
        else if (prop == "opacity")  { cl.opacity    = jval_float(val); }
        else if (prop == "muted")    { cl.muted      = jval_bool(val); }
        else if (prop == "in_point") { cl.in_point   = jval_float(val); }
        else if (prop == "fade_in")  { cl.fade_in    = jval_float(val); }
        else if (prop == "fade_out") { cl.fade_out   = jval_float(val); }
        else if (prop == "blend_mode") { cl.blend_mode = jval_int(val); }
        // ── Transform props ──────────────────────────────────────────────────
        else if (prop == "pos_x")    { cl.pos_x    = jval_float(val); }
        else if (prop == "pos_y")    { cl.pos_y    = jval_float(val); }
        else if (prop == "scale_x")  { cl.scale_x  = jval_float(val); }
        else if (prop == "scale_y")  { cl.scale_y  = jval_float(val); }
        else if (prop == "rotation") { cl.rotation = jval_float(val); }
        // Crop: clamp against the opposite side so a sliver stays visible
        else if (prop == "crop_l")   { cl.crop_l = fmaxf(0.f, fminf(jval_float(val), 0.95f - cl.crop_r)); }
        else if (prop == "crop_r")   { cl.crop_r = fmaxf(0.f, fminf(jval_float(val), 0.95f - cl.crop_l)); }
        else if (prop == "crop_t")   { cl.crop_t = fmaxf(0.f, fminf(jval_float(val), 0.95f - cl.crop_b)); }
        else if (prop == "crop_b")   { cl.crop_b = fmaxf(0.f, fminf(jval_float(val), 0.95f - cl.crop_t)); }
        // ── Text props ───────────────────────────────────────────────────────
        else if (prop == "text")       { cl.text      = val.get<std::string>(); }
        else if (prop == "font_size")  { cl.font_size = jval_float(val); }
        else if (prop == "sub_pos")    { cl.sub_pos   = jval_int(val); }
        else if (prop == "sub_pos_x")  { cl.sub_pos_x = jval_float(val); }
        else if (prop == "sub_pos_y")  { cl.sub_pos_y = jval_float(val); }
        else if (prop == "sub_anchor_h") { cl.sub_anchor_h = jval_int(val); }
        else if (prop == "sub_wrap_w") { cl.sub_wrap_w = jval_float(val); }
        else if (prop == "karaoke")    { cl.karaoke   = jval_bool(val); }
        else if (prop == "clip_style") { cl.clip_style = parse_anim_style(val.get<std::string>()); }
        else if (prop == "sub_color") {
            if (!val.is_array() || val.size() != 4) { err = "sub_color must be [r,g,b,a]"; return {}; }
            for (int i = 0; i < 4; ++i) cl.sub_color[i] = jval_float(val[i]);
            cl.sub_color_override = true;
        }
        else if (prop == "karaoke_highlight_color") {
            if (!val.is_array() || val.size() != 4) { err = "karaoke_highlight_color must be [r,g,b,a]"; return {}; }
            for (int i = 0; i < 4; ++i) cl.karaoke_highlight_color[i] = jval_float(val[i]);
        }
        // ── Callout props ────────────────────────────────────────────────────
        else if (prop == "callout_style") { cl.callout_style = jval_int(val); }
        else if (prop == "callout_arrow") { cl.callout_arrow = jval_bool(val); }
        else if (prop == "arrow_tx")      { cl.arrow_tx      = jval_float(val); }
        else if (prop == "arrow_ty")      { cl.arrow_ty      = jval_float(val); }
        // ── BodyFX props ─────────────────────────────────────────────────────
        else if (prop == "body_fx_type") {
            std::string name = val.get<std::string>();
            int n = body_fx_info_count();
            const BodyFXInfo* infos = body_fx_info_list();
            bool found = false;
            for (int i = 0; i < n; ++i) {
                if (infos[i].name == name) {
                    cl.body_fx_type = infos[i].type;
                    for (int pi = 0; pi < 4; ++pi)
                        cl.body_fx_params[pi] = pi < infos[i].n_params ? infos[i].params[pi].default_val : 0.5f;
                    found = true; break;
                }
            }
            if (!found) { err = "unknown body_fx_type: " + name; return {}; }
        }
        else if (prop == "body_fx_amount")  { cl.body_fx_amount     = jval_float(val); }
        else if (prop == "body_fx_param_0") { cl.body_fx_params[0]  = jval_float(val); }
        else if (prop == "body_fx_param_1") { cl.body_fx_params[1]  = jval_float(val); }
        else if (prop == "body_fx_param_2") { cl.body_fx_params[2]  = jval_float(val); }
        else if (prop == "body_fx_param_3") { cl.body_fx_params[3]  = jval_float(val); }
        else if (prop == "grade_brightness") { cl.grade_brightness = jval_float(val); }
        else if (prop == "grade_contrast")   { cl.grade_contrast   = jval_float(val); }
        else if (prop == "grade_saturation") { cl.grade_saturation = jval_float(val); }
        else if (prop == "grade_hue")        { cl.grade_hue        = jval_float(val); }
        else { err = "unknown prop: " + prop; return {}; }
        return json::object();
    }

    if (method == "add_track") {
        std::string name = params.value("name", "Track");
        int pos = params.value("position", 0);
        if (pos < 0) pos = 0;
        if (pos > (int)state.tracks.size()) pos = (int)state.tracks.size();
        Track t;
        t.name = name;
        state.tracks.insert(state.tracks.begin() + pos, t);
        json r; r["track"] = pos;
        return r;
    }

    if (method == "delete_track") {
        int ti = track_by_name_or_index(state, params);
        if (!check_track(state, ti, err)) return {};
        state.tracks.erase(state.tracks.begin() + ti);
        if (state.selected_track == ti) { state.selected_track = -1; state.selected_clip = -1; }
        return json::object();
    }

    if (method == "add_marker") {
        float mtime  = params.value("time", 0.f);
        std::string mlabel = params.value("label", "");
        uint32_t mcolor = 0xFF4A90E2u;
        if (params.contains("color") && params["color"].is_string()) {
            std::string cs = params["color"].get<std::string>();
            if (!cs.empty() && cs[0] == '#') cs = cs.substr(1);
            mcolor = (uint32_t)std::stoul(cs, nullptr, 16) | 0xFF000000u;
        }
        Marker m;
        m.time  = mtime;
        m.label = mlabel;
        m.color = mcolor;
        auto it = state.markers.begin();
        while (it != state.markers.end() && it->time <= mtime) ++it;
        int idx = (int)(it - state.markers.begin());
        state.markers.insert(it, m);
        json r; r["index"] = idx;
        return r;
    }

    if (method == "remove_marker") {
        int mi = params.value("index", -1);
        if (mi < 0 || mi >= (int)state.markers.size()) { err = "invalid marker index"; return {}; }
        state.markers.erase(state.markers.begin() + mi);
        return json::object();
    }

    if (method == "get_markers") {
        json arr = json::array();
        for (int mi = 0; mi < (int)state.markers.size(); ++mi) {
            const auto& m = state.markers[mi];
            char hex[12];
            snprintf(hex, sizeof(hex), "#%06X", m.color & 0x00FFFFFFu);
            json mj;
            mj["index"] = mi;
            mj["time"]  = m.time;
            mj["label"] = m.label;
            mj["color"] = hex;
            arr.push_back(mj);
        }
        json r; r["markers"] = arr;
        return r;
    }

    if (method == "trigger_pipeline") {
        std::string mode_s = params.value("mode", "both");
        PipelineMode mode = PipelineMode::Both;
        if (mode_s == "transcribe_only") mode = PipelineMode::TranscribeOnly;
        else if (mode_s == "separate_only") mode = PipelineMode::SeparateOnly;
        std::string src = params.value("path", "");
        if (!src.empty()) state.audio_path = src;
        if (state.audio_path.empty()) { err = "no audio file loaded"; return {}; }
        // MCP callers expect the transcript only — no surprise lyric clips on the
        // timeline. Leaving pipeline_on_done null means the completion handler
        // does its data-loading work and stops there.
        state.pipeline_on_done = {};
        kick_pipeline(state, state.audio_path, mode);
        // Non-blocking: return immediately with stage=running so the caller can
        // poll get_pipeline_status. The pipeline runs in the background thread
        // launched by kick_pipeline.
        json r;
        r["stage"]    = "running";
        r["progress"] = state.pipeline.progress;
        r["next_action_hint"] = "Poll get_pipeline_status every 2-3s until stage='done', then call generate_typography(preset='flash'|'apple'|'spotify'|'karaoke'|'headline'|...) to lay lyric clips. Do NOT hand-roll text clips with add_clip(type='text') for lyrics — generate_typography handles styling, grouping and idempotency.";
        return r;
    }

    if (method == "generate_typography") {
        std::string preset = params.value("preset", "");
        if (!preset.empty()) state.typo_preset_id = preset;
        generate_typography(state);
        json r; r["preset"] = state.typo_preset_id; return r;
    }

    if (method == "load_project") {
        std::string path = params.value("path", "");
        if (path.empty()) { err = "path is required"; return {}; }
        bool mr = state.models_ready;
        bool ms = state.models_skipped;
        if (!project_load(state, path)) { err = "project_load failed"; return {}; }
        state.models_ready   = mr;
        state.models_skipped = ms;
        state.proxy_scan_needed = true;
        // Fresh history with a baseline snapshot: the old project's entries
        // must not bleed into this one, and history_undo() can't step back
        // past entry 0, so the first edit needs a predecessor to restore.
        history_clear();
        history_push(state, "Load project");
        json r; r["path"] = path;
        return r;
    }

    if (method == "set_text_style") {
        int ti = params.value("track", -1), ci = params.value("clip", -1);
        if (!check_clip(state, ti, ci, err)) return {};
        TextStyle& ts = state.tracks[ti].clips[ci].ts;
        auto get4 = [&](const char* key, float* dst) {
            if (params.contains(key) && params[key].is_array() && params[key].size() == 4)
                for (int i = 0; i < 4; ++i) dst[i] = params[key][i].get<float>();
        };
        if (params.contains("shadow_enabled")) ts.shadow_enabled = params["shadow_enabled"].get<bool>();
        if (params.contains("shadow_ox"))      ts.shadow_ox      = params["shadow_ox"].get<float>();
        if (params.contains("shadow_oy"))      ts.shadow_oy      = params["shadow_oy"].get<float>();
        get4("shadow_col", ts.shadow_col);
        if (params.contains("stroke_enabled")) ts.stroke_enabled = params["stroke_enabled"].get<bool>();
        if (params.contains("stroke_w"))       ts.stroke_w       = params["stroke_w"].get<float>();
        get4("stroke_col", ts.stroke_col);
        if (params.contains("glow_enabled"))   ts.glow_enabled   = params["glow_enabled"].get<bool>();
        if (params.contains("glow_r"))         ts.glow_r         = params["glow_r"].get<float>();
        get4("glow_col", ts.glow_col);
        if (params.contains("bg_enabled"))     ts.bg_enabled     = params["bg_enabled"].get<bool>();
        get4("bg_col", ts.bg_col);
        if (params.contains("bg_pad_x"))       ts.bg_pad_x       = params["bg_pad_x"].get<float>();
        if (params.contains("bg_pad_y"))       ts.bg_pad_y       = params["bg_pad_y"].get<float>();
        if (params.contains("bg_corner"))      ts.bg_corner      = params["bg_corner"].get<float>();
        return json::object();
    }

    if (method == "set_clip_fx") {
        int ti = params.value("track", -1), ci = params.value("clip", -1);
        if (!check_clip(state, ti, ci, err)) return {};
        std::string fx_id = params.value("fx_id", "");
        if (fx_id.empty()) { err = "fx_id is required"; return {}; }
        Clip& cl = state.tracks[ti].clips[ci];
        if (params.contains("amount"))
            fx_clip_set_param(cl, fx_id, "amount", params["amount"].get<float>());
        if (params.contains("params") && params["params"].is_object()) {
            for (auto& [pname, pval] : params["params"].items()) {
                if (!pval.is_number()) continue;
                if (!fx_clip_set_param(cl, fx_id, pname, pval.get<float>())) {
                    err = "unknown param '" + pname + "' for effect '" + fx_id + "'";
                    return {};
                }
            }
        }
        // Set FX type so the effect clip renders
        cl.fx_type = FXType::Grade;  // will be overridden if clip is Effect type
        return json::object();
    }

    if (method == "add_effect_brick") {
        int ti = track_by_name_or_index(state, params);
        if (ti < 0 || ti >= (int)state.tracks.size()) { err = "track index out of range"; return {}; }
        if (state.tracks[ti].locked) { err = "track is locked"; return {}; }
        std::string fx_s = params.value("fx_type", "grade");
        float start = snap_to_frame(params.value("start", 0.f), state.fps);
        float end   = snap_end_to_frame(params.value("end", start + 2.f), state.fps);

        Clip cl;
        cl.clip_type = ClipType::Effect;
        cl.fx_type   = parse_fx_type(fx_s);
        cl.start     = start;
        cl.end       = end;

        if (params.contains("params") && params["params"].is_object()) {
            if (!apply_effect_params(cl, params["params"], fx_s, err)) return {};
        }

        state.tracks[ti].clips.push_back(cl);
        json r; r["clip"] = (int)state.tracks[ti].clips.size() - 1;
        return r;
    }

    if (method == "add_multifx_brick") {
        int ti = track_by_name_or_index(state, params);
        if (!check_track(state, ti, err)) return {};
        if (state.tracks[ti].locked) { err = "track is locked"; return {}; }
        float start = snap_to_frame(params.value("start", 0.f), state.fps);
        float end   = snap_end_to_frame(params.value("end", start + 2.f), state.fps);

        Clip brick;
        brick.clip_type = ClipType::MultiFX;
        brick.start     = start;
        brick.end       = end;

        if (params.contains("effects") && params["effects"].is_array()) {
            float dur = end - start;
            for (auto& e : params["effects"]) {
                Clip se;
                std::string fxt = e.value("fx_type", "grade");
                se.rel_start = e.value("rel_start", 0.f);
                se.rel_end   = e.value("rel_end",   0.f);
                if (se.rel_end > dur) se.rel_end = dur;

                if (fxt == "body_fx") {
                    se.clip_type = ClipType::BodyFX;
                    std::string bname = e.value("body_fx_type", "NeonOutline");
                    int n_body = body_fx_info_count();
                    const BodyFXInfo* infos = body_fx_info_list();
                    bool found = false;
                    for (int i = 0; i < n_body; ++i) {
                        if (infos[i].name == bname) {
                            se.body_fx_type = infos[i].type;
                            for (int pi = 0; pi < 4; ++pi)
                                se.body_fx_params[pi] = pi < infos[i].n_params ? infos[i].params[pi].default_val : 0.5f;
                            found = true; break;
                        }
                    }
                    if (!found) { err = "unknown body_fx_type: " + bname; return {}; }
                } else {
                    se.clip_type = ClipType::Effect;
                    se.fx_type   = parse_fx_type(fxt);
                    if (e.contains("params") && e["params"].is_object()) {
                        if (!apply_effect_params(se, e["params"], fxt, err)) return {};
                    }
                }
                brick.fx_chain.push_back(se);
            }
        }

        state.tracks[ti].clips.push_back(brick);
        json r; r["clip"] = (int)state.tracks[ti].clips.size() - 1;
        return r;
    }

    if (method == "new_project") {
        bool mr = state.models_ready;
        bool ms = state.models_skipped;
        state = AppState{};
        state.models_ready   = mr;
        state.models_skipped = ms;
        state.splash_timer   = 0.f;
        // Fresh history + baseline snapshot (same reasoning as load_project).
        history_clear();
        history_push(state, "New project");
        return json::object();
    }

    if (method == "rename_track") {
        int ti = track_by_name_or_index(state, params);
        if (!check_track(state, ti, err)) return {};
        state.tracks[ti].name = params.value("name", state.tracks[ti].name);
        return json::object();
    }

    if (method == "set_format") {
        std::string fmt = params.value("format", "");
        if      (fmt == "vertical"   || fmt == "9:16") state.format = OutputFormat::Vertical;
        else if (fmt == "horizontal" || fmt == "16:9") state.format = OutputFormat::Horizontal;
        else if (fmt == "square"     || fmt == "1:1")  state.format = OutputFormat::Square;
        else { err = "unknown format: " + fmt + " (use vertical/9:16, horizontal/16:9, square/1:1)"; return {}; }
        history_push(state, "Set format: " + fmt);
        return json::object();
    }

    if (method == "trim_all_to") {
        double t = params.value("time", -1.0);
        if (t < 0.0) { err = "time is required"; return {}; }
        for (auto& tr : state.tracks) {
            std::vector<Clip> kept;
            for (auto& cl : tr.clips) {
                if (cl.start >= t) continue;          // delete clips starting at or after t
                if (cl.end > t) cl.end = (float)t;   // clamp clips that straddle t
                kept.push_back(cl);
            }
            tr.clips = std::move(kept);
        }
        history_push(state, "trim_all_to");
        json r; r["time"] = t; return r;
    }

    if (method == "delete_clips_after") {
        int ti = track_by_name_or_index(state, params);
        if (!check_track(state, ti, err)) return {};
        double t = params.value("time", -1.0);
        if (t < 0.0) { err = "time is required"; return {}; }
        auto& clips = state.tracks[ti].clips;
        clips.erase(std::remove_if(clips.begin(), clips.end(),
            [t](const Clip& c){ return c.start >= (float)t; }), clips.end());
        history_push(state, "delete_clips_after");
        return json::object();
    }

    if (method == "get_stills") {
        if (!params.contains("paths") || !params["paths"].is_array()) {
            err = "paths array required"; return {};
        }
        std::vector<std::string> paths;
        for (auto& p : params["paths"]) paths.push_back(p.get<std::string>());
        if (client_fd < 0) { return json::object(); }
        fd_mark_busy(client_fd);
        std::thread([paths, client_fd, req_id]() {
            json stills = json::array();
            for (const auto& p : paths) {
                proxy_ensure_still(p);
                json entry;
                entry["path"]  = p;
                entry["still"] = proxy_still_path(p);
                entry["ok"]    = std::filesystem::exists(proxy_still_path(p));
                stills.push_back(std::move(entry));
            }
            json r; r["stills"] = stills;
            send_ok_id(client_fd, req_id, r);
            agent_done();
            fd_mark_free(client_fd);
        }).detach();
        json sentinel; sentinel["__async"] = true; return sentinel;
    }

    err = "unknown method: " + method;
    return {};
}

// ── Per-client message processing ─────────────────────────────────────────────

static void process_client(Client& cl, AppState& state) {
    if (fd_is_busy(cl.fd)) return;  // waiting for async background op to send its response

    // Try to read more data
    char buf[4096];
    ssize_t n = read(cl.fd, buf, sizeof(buf));
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        // Connection closed or error
        if (g_in_batch && g_batch_state == &state) {
            history_push(state, g_batch_label + " (incomplete)");
            g_in_batch     = false;
            g_auto_batched = false;
            g_batch_label.clear();
            g_batch_state  = nullptr;
        }
        close(cl.fd);
        cl.fd = -1;
        return;
    }
    if (n > 0) cl.rbuf.append(buf, (size_t)n);

    // Process complete lines
    size_t pos;
    while ((pos = cl.rbuf.find('\n')) != std::string::npos) {
        std::string line = cl.rbuf.substr(0, pos);
        cl.rbuf.erase(0, pos + 1);
        if (line.empty()) continue;

        std::string req_id;
        try {
            json req = json::parse(line);
            req_id = req.value("id", "");
            std::string method = req.value("method", "");
            json params = req.value("params", json::object());

            std::string err;
            // agent_status manages agent_active itself — wrapping it in
            // begin/done would clear the message it just set.
            bool manages_agent = (method == "agent_status");
            if (!manages_agent) agent_begin(state, method);
            json result = dispatch(state, method, params, err, cl.fd, req_id);
            bool is_async = result.is_object() && result.value("__async", false);
            if (!is_async) {
                // Close auto-batch and attach state diff
                if (g_auto_batched && g_in_batch) {
                    history_push(state, g_batch_label);
                    g_in_batch     = false;
                    g_auto_batched = false;
                    g_batch_label.clear();
                    g_batch_state  = nullptr;
                    if (err.empty()) result = state_to_json(state);
                }
                if (!err.empty()) {
                    send_err_id(cl.fd, req_id, err);
                } else {
                    send_ok_id(cl.fd, req_id, result);
                }
                if (!manages_agent) agent_done();
            }
        } catch (const json::exception& e) {
            json r; r["id"] = req_id; r["error"] = std::string("JSON parse error: ") + e.what();
            send_json(cl.fd, r);
        } catch (const std::exception& e) {
            json r; r["id"] = req_id; r["error"] = std::string("internal error: ") + e.what();
            send_json(cl.fd, r);
        }
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

void ipc_server_start() {
    g_sock_path = "/tmp/pop-maker-studio.sock";

    // Remove stale socket
    ::unlink(g_sock_path.c_str());

    g_srv_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_srv_fd < 0) { perror("[ipc] socket"); return; }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, g_sock_path.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(g_srv_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("[ipc] bind"); ::close(g_srv_fd); g_srv_fd = -1; return;
    }
    if (::listen(g_srv_fd, 8) < 0) {
        perror("[ipc] listen"); ::close(g_srv_fd); g_srv_fd = -1; return;
    }
    set_nonblock(g_srv_fd);

    fprintf(stdout, "[ipc] listening on %s\n", g_sock_path.c_str());
}

void ipc_server_poll(AppState& state) {
    if (g_srv_fd < 0) return;

    // Accept new connections
    for (;;) {
        int fd = ::accept(g_srv_fd, nullptr, nullptr);
        if (fd < 0) break;
        set_nonblock(fd);
        g_clients.push_back({fd, {}});
    }

    // Process existing clients
    for (auto& cl : g_clients) {
        if (cl.fd >= 0) process_client(cl, state);
    }

    // Remove disconnected clients
    g_clients.erase(
        std::remove_if(g_clients.begin(), g_clients.end(),
                       [](const Client& c){ return c.fd < 0; }),
        g_clients.end());
}

void ipc_server_stop() {
    for (auto& cl : g_clients) if (cl.fd >= 0) { ::close(cl.fd); cl.fd = -1; }
    g_clients.clear();

    if (g_srv_fd >= 0) { ::close(g_srv_fd); g_srv_fd = -1; }
    if (!g_sock_path.empty()) ::unlink(g_sock_path.c_str());

    if (g_in_batch) { g_in_batch = false; g_auto_batched = false; g_batch_state = nullptr; }
}
