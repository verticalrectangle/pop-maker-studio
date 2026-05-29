#include "ipc_server.h"
#include "history.h"
#include "runtime_fx.h"
#include "ui/pipeline.h"
#include "ui/panel_animation.h"
#include "project.h"
#include "beat_detect.h"
#include "generated/fx_clip_set_dispatch.h"
#include "json.hpp"

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
#include <thread>
#include <fstream>

using json = nlohmann::json;

// ── Static state ──────────────────────────────────────────────────────────────

static struct {
    std::atomic<bool> running{false};
    std::atomic<bool> done{false};
    BeatResult        result;
} s_audio_analysis;

static int  g_srv_fd   = -1;
static std::string g_sock_path;
static std::string g_lock_path = "/tmp/pop-maker-studio.lock";

struct Client {
    int  fd = -1;
    std::string rbuf;  // accumulates partial reads
};
static std::vector<Client> g_clients;

// Batch state: IPC edits are rejected unless inside a batch.
static bool        g_in_batch    = false;
static std::string g_batch_label;
static AppState*   g_batch_state = nullptr;  // state pointer for end_batch

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
    return FXType::Grade;
}

// Set fx-specific params on an Effect clip from a string-keyed params object.
// Covers all built-in FX types; returns false and sets err for unknown keys.
static bool apply_effect_params(Clip& cl, const json& params, std::string& err) {
    for (auto& [k, v] : params.items()) {
        if (!v.is_number()) { err = "param '" + k + "' must be a number"; return false; }
        float fv = v.get<float>();
        // Grade
        if (k == "brightness")   { cl.fx_color_on = true; cl.fx_brightness  = fv; }
        else if (k == "contrast")     { cl.fx_color_on = true; cl.fx_contrast    = fv; }
        else if (k == "saturation")   { cl.fx_color_on = true; cl.fx_saturation  = fv; }
        else if (k == "hue")          { cl.fx_color_on = true; cl.fx_hue         = fv; }
        // Blur
        else if (k == "blur")         { cl.fx_blur_on  = true; cl.fx_blur        = fv; }
        // Vignette
        else if (k == "vignette")     { cl.fx_vignette_on = true; cl.fx_vignette = fv; }
        // Text override
        else if (k == "opacity_mul")  { cl.fx_text_on  = true; cl.fx_opacity_mul = fv; }
        else if (k == "scale_mul")    { cl.fx_text_on  = true; cl.fx_scale_mul   = fv; }
        // Glitch
        else if (k == "glitch_chroma")            { cl.fx_glitch_chroma            = fv; }
        else if (k == "glitch_jitter")            { cl.fx_glitch_jitter            = fv; }
        else if (k == "glitch_corruption")        { cl.fx_glitch_corruption        = fv; }
        else if (k == "glitch_corruption_bleed")  { cl.fx_glitch_corruption_bleed  = fv; }
        // ZoomPunch
        else if (k == "zoom_strength") { cl.fx_zoom_strength = fv; }
        else if (k == "zoom_decay")    { cl.fx_zoom_decay    = fv; }
        else if (k == "zoom_shake")    { cl.fx_zoom_shake    = fv; }
        // LightLeak
        else if (k == "leak_intensity") { cl.fx_leak_intensity = fv; }
        else if (k == "leak_speed")     { cl.fx_leak_speed     = fv; }
        // VHS
        else if (k == "vhs_noise")    { cl.fx_vhs_noise    = fv; }
        else if (k == "vhs_bleed")    { cl.fx_vhs_bleed    = fv; }
        else if (k == "vhs_tracking") { cl.fx_vhs_tracking = fv; }
        // Datamosh
        else if (k == "datamosh_intensity") { cl.fx_datamosh_intensity = fv; }
        else if (k == "datamosh_spread")    { cl.fx_datamosh_spread    = fv; }
        // ChromaKey
        else if (k == "chroma_key_r")         { cl.fx_chroma_key_r         = fv; }
        else if (k == "chroma_key_g")         { cl.fx_chroma_key_g         = fv; }
        else if (k == "chroma_key_b")         { cl.fx_chroma_key_b         = fv; }
        else if (k == "chroma_key_threshold") { cl.fx_chroma_key_threshold = fv; }
        else if (k == "chroma_key_softness")  { cl.fx_chroma_key_softness  = fv; }
        else { err = "unknown effect param: " + k; return false; }
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

    if (!c.runtime_fx_id.empty()) {
        j["runtime_fx_id"]     = c.runtime_fx_id;
        j["runtime_fx_amount"] = c.runtime_fx_amount;
    }
    return j;
}

static json state_to_json(const AppState& state) {
    json j;
    j["duration"]   = state.duration;
    j["fps"]        = state.fps;
    j["bpm"]        = state.beat_bpm;
    j["audio_path"] = state.audio_path;
    j["playhead"]   = state.playhead;

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

// ── Command dispatch ──────────────────────────────────────────────────────────

static json dispatch(AppState& state, const std::string& method, const json& params, std::string& err) {
    // ── Read-only: no batch required ─────────────────────────────────────────
    if (method == "get_project") {
        return state_to_json(state);
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
        return j;
    }

    if (method == "analyze_audio") {
        std::string path = params.value("path", "");
        if (path.empty()) { err = "path required"; return {}; }
        if (s_audio_analysis.running.load()) { err = "analysis already running"; return {}; }
        s_audio_analysis.running.store(true);
        s_audio_analysis.done.store(false);
        std::thread([path]() {
            s_audio_analysis.result = beat_detect(path);
            s_audio_analysis.done.store(true);
            s_audio_analysis.running.store(false);
        }).detach();
        json r; r["status"] = "started";
        return r;
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
        return json::object();
    }

    if (method == "end_batch") {
        if (!g_in_batch) { err = "not in a batch"; return {}; }
        history_push(state, g_batch_label);
        g_in_batch    = false;
        g_batch_label.clear();
        g_batch_state = nullptr;
        return json::object();
    }

    // ── Editing: require batch ────────────────────────────────────────────────
    if (!g_in_batch) {
        err = "editing calls must be inside a begin_batch / end_batch";
        return {};
    }

    if (method == "move_clip") {
        int ti = params.value("track", -1), ci = params.value("clip", -1);
        float start = params.value("start", 0.f);
        if (!check_clip(state, ti, ci, err)) return {};
        Clip& cl = state.tracks[ti].clips[ci];
        float dur = cl.end - cl.start;
        cl.start = start;
        cl.end   = start + dur;
        return json::object();
    }

    if (method == "trim_clip") {
        int ti = params.value("track", -1), ci = params.value("clip", -1);
        if (!check_clip(state, ti, ci, err)) return {};
        Clip& cl = state.tracks[ti].clips[ci];
        float old_start = cl.start;
        if (params.contains("start")) {
            float ns = params["start"].get<float>();
            float delta = ns - old_start;
            if (delta > 0.f) cl.in_point += delta;
            cl.start = ns;
        }
        if (params.contains("end")) cl.end = params["end"].get<float>();
        return json::object();
    }

    if (method == "split_clip") {
        int ti = params.value("track", -1), ci = params.value("clip", -1);
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
        int ti = params.value("track", -1), ci = params.value("clip", -1);
        if (!check_clip(state, ti, ci, err)) return {};
        state.tracks[ti].clips.erase(state.tracks[ti].clips.begin() + ci);
        if (state.selected_track == ti && state.selected_clip == ci)
            state.selected_clip = -1;
        return json::object();
    }

    if (method == "add_clip") {
        int ti = params.value("track", -1);
        if (!check_track(state, ti, err)) return {};
        if (state.tracks[ti].locked) { err = "track is locked"; return {}; }
        std::string type_s = params.value("type", "text");
        float start = params.value("start", 0.f);
        float end   = params.value("end", start + 2.f);
        std::string text = params.value("text", "");

        Clip cl;
        cl.clip_type = parse_clip_type(type_s);
        cl.start = start;
        cl.end   = end;
        cl.text  = text;
        if (cl.clip_type == ClipType::Video || cl.clip_type == ClipType::Audio)
            cl.source_id = text;
        state.tracks[ti].clips.push_back(cl);
        int new_ci = (int)state.tracks[ti].clips.size() - 1;
        if (cl.clip_type == ClipType::Video) state.proxy_scan_needed = true;
        json r; r["clip"] = new_ci;
        return r;
    }

    if (method == "set_clip_prop") {
        int ti = params.value("track", -1), ci = params.value("clip", -1);
        std::string prop = params.value("prop", "");
        if (!check_clip(state, ti, ci, err)) return {};
        Clip& cl = state.tracks[ti].clips[ci];

        auto& val = params["value"];
        // ── A/V props ────────────────────────────────────────────────────────
        if      (prop == "volume")   { cl.volume     = jval_float(val); }
        else if (prop == "speed")    { cl.speed      = jval_float(val); }
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
        int ti = params.value("track", -1);
        if (!check_track(state, ti, err)) return {};
        state.tracks.erase(state.tracks.begin() + ti);
        if (state.selected_track == ti) { state.selected_track = -1; state.selected_clip = -1; }
        return json::object();
    }

    if (method == "trigger_pipeline") {
        std::string mode_s = params.value("mode", "both");
        PipelineMode mode = PipelineMode::Both;
        if (mode_s == "transcribe_only") mode = PipelineMode::TranscribeOnly;
        else if (mode_s == "separate_only") mode = PipelineMode::SeparateOnly;
        if (state.audio_path.empty()) { err = "no audio file loaded"; return {}; }
        kick_pipeline(state, state.audio_path, mode);
        return json::object();
    }

    if (method == "generate_typography") {
        generate_typography(state);
        return json::object();
    }

    if (method == "load_project") {
        std::string path = params.value("path", "");
        if (path.empty()) { err = "path is required"; return {}; }
        if (!project_load(state, path)) { err = "project_load failed"; return {}; }
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
        int ti = params.value("track", -1);
        if (ti < 0 || ti >= (int)state.tracks.size()) { err = "track index out of range"; return {}; }
        if (state.tracks[ti].locked) { err = "track is locked"; return {}; }
        std::string fx_s = params.value("fx_type", "grade");
        float start = params.value("start", 0.f);
        float end   = params.value("end",   start + 2.f);

        Clip cl;
        cl.clip_type = ClipType::Effect;
        cl.fx_type   = parse_fx_type(fx_s);
        cl.start     = start;
        cl.end       = end;

        if (params.contains("params") && params["params"].is_object()) {
            if (!apply_effect_params(cl, params["params"], err)) return {};
        }

        state.tracks[ti].clips.push_back(cl);
        json r; r["clip"] = (int)state.tracks[ti].clips.size() - 1;
        return r;
    }

    if (method == "add_multifx_brick") {
        int ti = params.value("track", -1);
        if (!check_track(state, ti, err)) return {};
        if (state.tracks[ti].locked) { err = "track is locked"; return {}; }
        float start = params.value("start", 0.f);
        float end   = params.value("end",   start + 2.f);

        Clip brick;
        brick.clip_type = ClipType::MultiFX;
        brick.start     = start;
        brick.end       = end;

        if (params.contains("effects") && params["effects"].is_array()) {
            float dur = end - start;
            for (auto& e : params["effects"]) {
                Clip se;
                se.clip_type  = ClipType::Effect;
                se.fx_type    = parse_fx_type(e.value("fx_type", "grade"));
                se.rel_start  = e.value("rel_start", 0.f);
                se.rel_end    = e.value("rel_end",   0.f);
                // clamp rel_end to parent duration
                if (se.rel_end > dur) se.rel_end = dur;
                if (e.contains("params") && e["params"].is_object()) {
                    if (!apply_effect_params(se, e["params"], err)) return {};
                }
                brick.fx_chain.push_back(se);
            }
        }

        state.tracks[ti].clips.push_back(brick);
        json r; r["clip"] = (int)state.tracks[ti].clips.size() - 1;
        return r;
    }

    if (method == "new_project") {
        state = AppState{};
        return json::object();
    }

    if (method == "rename_track") {
        int ti = params.value("track", -1);
        if (!check_track(state, ti, err)) return {};
        state.tracks[ti].name = params.value("name", state.tracks[ti].name);
        return json::object();
    }

    err = "unknown method: " + method;
    return {};
}

// ── Per-client message processing ─────────────────────────────────────────────

static void process_client(Client& cl, AppState& state) {
    // Try to read more data
    char buf[4096];
    ssize_t n = read(cl.fd, buf, sizeof(buf));
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        // Connection closed or error
        if (g_in_batch && g_batch_state == &state) {
            // Close open batch: push history and reset
            history_push(state, g_batch_label + " (incomplete)");
            g_in_batch    = false;
            g_batch_label.clear();
            g_batch_state = nullptr;
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
            json result = dispatch(state, method, params, err);
            if (!err.empty()) {
                send_err_id(cl.fd, req_id, err);
            } else {
                send_ok_id(cl.fd, req_id, result);
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
    // Build socket path using PID
    char sock_buf[128];
    snprintf(sock_buf, sizeof(sock_buf), "/tmp/pop-maker-studio-%d.sock", (int)getpid());
    g_sock_path = sock_buf;

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

    // Write lock file
    {
        std::ofstream lf(g_lock_path);
        lf << getpid() << " " << g_sock_path << "\n";
    }

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
    ::unlink(g_lock_path.c_str());

    if (g_in_batch) { g_in_batch = false; g_batch_state = nullptr; }
}
