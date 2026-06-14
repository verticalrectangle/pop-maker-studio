#include "ipc_server.h"
#include "audio.h"
#include "recorder.h"
#include "video_recorder.h"
#include "face_track.h"
#include "agent_harness.h"
#include "render.h"
#include "bg_presets.h"
#include "bg_remove.h"
#include "history.h"
#include "runtime_fx.h"
#include "ui/canvas.h"
#include "ui/timeline.h"
#include "ui/pipeline.h"
#include "ui/screens.h"   // recent_projects_push
#include "ui/panel_animation.h"
#include "ui/panel_media.h"
#include "ui/studio_shared.h"
#include "project.h"
#include "beat_detect.h"
#include "scene_detect.h"
#include "paths.h"
#include "vision_caption.h"
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
#include <deque>
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
        case ClipType::AudioMultiFX: return "audio_multi_fx";
        case ClipType::Record:     return "record";
        case ClipType::VideoRecord: return "video_record";
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
    if (s == "audio_autotune") return FXType::AudioAutotune;
    if (s == "audio_pitch")    return FXType::AudioPitch;
    if (s == "audio_formant")  return FXType::AudioFormant;
    if (s == "audio_delay")    return FXType::AudioDelay;
    if (s == "audio_reverb")   return FXType::AudioReverb;
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

static const char* interp_str(InterpType it) {
    switch (it) {
        case InterpType::Linear:  return "linear";
        case InterpType::EaseIn:  return "ease_in";
        case InterpType::EaseOut: return "ease_out";
        case InterpType::Hold:    return "hold";
        default:                  return "ease_both";
    }
}

static InterpType interp_from_str(const std::string& s) {
    if (s == "linear")   return InterpType::Linear;
    if (s == "ease_in")  return InterpType::EaseIn;
    if (s == "ease_out") return InterpType::EaseOut;
    if (s == "hold")     return InterpType::Hold;
    return InterpType::EaseBoth;
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
    if (c.clip_type == ClipType::Record ||
        c.clip_type == ClipType::VideoRecord) {
        j["takes"]         = c.rec_takes;
        j["selected_take"] = c.rec_take_sel;
    }
    if (c.fx_coupled) j["coupled"] = true;
    if (c.clip_type == ClipType::Effect && fx_type_is_audio_fx(c.fx_type)) {
        json a;
        a["autotune_on"] = c.audio_fx.autotune_on;
        a["pitch_on"]    = c.audio_fx.pitch_on;
        a["pitch_semitones"] = c.audio_fx.pitch_semitones;
        a["formant_on"]  = c.audio_fx.formant_on;
        a["delay_on"]    = c.audio_fx.delay_on;
        a["reverb_on"]   = c.audio_fx.reverb_on;
        j["audio_fx"]    = a;
    }
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
    if (!c.ktracks.empty()) {
        json kfs = json::object();
        for (auto& [prop, pt] : c.ktracks) {
            if (pt.empty()) continue;
            json arr = json::array();
            for (auto& k : pt.keys)
                arr.push_back({{"t", k.time}, {"v", k.value},
                               {"interp", interp_str(k.interp)}});
            kfs[prop] = std::move(arr);
        }
        if (!kfs.empty()) j["keyframes"] = std::move(kfs);
    }
    if (c.clip_type == ClipType::Record ||
        c.clip_type == ClipType::VideoRecord) {
        j["takes"]         = c.rec_takes;
        j["selected_take"] = c.rec_take_sel;
    }
    if (c.fx_coupled) j["coupled"] = true;
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
    if (s == "record")    return ClipType::Record;
    if (s == "video_record") return ClipType::VideoRecord;
    return ClipType::Text;
}

// Background clips render nothing unless text is a valid preset id, so reject
// bad ids at creation instead of acking an invisible clip. On success, copy
// the preset's default speed/colors — same as the UI creation paths.
static bool apply_bg_preset(Clip& cl, const std::string& text, std::string& err) {
    if (cl.clip_type != ClipType::Background) return true;
    const BgPreset* pr = bg_preset_by_id(text.c_str());
    if (!pr) {
        std::string ids;
        for (int i = 0; i < g_n_bg_presets; ++i) {
            if (i) ids += ", ";
            ids += g_bg_presets[i].id;
        }
        err = (text.empty()
                   ? std::string("background clips need text = a preset id")
                   : "unknown background preset '" + text + "'") +
              ". For a flat color use 'solid', then set_clip_prop "
              "bg_c1=[r,g,b,a]. Valid ids: " + ids;
        return false;
    }
    cl.bg_speed = pr->default_speed;
    memcpy(cl.bg_c1, pr->dc1, sizeof cl.bg_c1);
    memcpy(cl.bg_c2, pr->dc2, sizeof cl.bg_c2);
    memcpy(cl.bg_c3, pr->dc3, sizeof cl.bg_c3);
    return true;
}

static bool clip_is_fx(const Clip& c) {
    return c.clip_type == ClipType::Effect ||
           c.clip_type == ClipType::MultiFX ||
           c.clip_type == ClipType::BodyFX;
}

// FX bricks never stack: overlapping effects belong in one MultiFX chain.
// Same invariant the timeline enforces with weld-or-bounce; FX over content
// stays legal (glass bricks ride over video). skip_ci: the clip being moved
// or trimmed, -1 when adding. Returns true and sets err on collision.
static bool fx_overlap_on_track(const AppState& state, int ti,
                                float start, float end, int skip_ci,
                                std::string& err) {
    for (int ci = 0; ci < (int)state.tracks[ti].clips.size(); ++ci) {
        if (ci == skip_ci) continue;
        const Clip& oc = state.tracks[ti].clips[ci];
        if (!clip_is_fx(oc)) continue;
        if (start < oc.end && end > oc.start) {
            char buf[224];
            snprintf(buf, sizeof(buf),
                     "FX bricks cannot overlap on a track: requested "
                     "[%.3fs \xe2\x80\x93 %.3fs] collides with FX clip %d "
                     "[%.3fs \xe2\x80\x93 %.3fs]. Combine the effects in one "
                     "add_multifx_brick chain, or use another track or time range.",
                     start, end, ci, oc.start, oc.end);
            err = buf;
            return true;
        }
    }
    return false;
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

// Auto-couple a freshly added video FX brick to the best-overlap content
// clip on its track — agents get the coupled end-state instantly (the UI's
// 1.5 s ring is a human affordance). Returns the brick's resulting index.
static int ipc_autocouple_fx(AppState& state, int ti, int ci) {
    auto& clips = state.tracks[ti].clips;
    if (ci < 0 || ci >= (int)clips.size()) return ci;
    Clip& c = clips[(size_t)ci];
    if (c.fx_coupled || !(fx_brick_is_video(c) || fx_brick_is_audio_kind(c)))
        return ci;
    const bool audio_kind = fx_brick_is_audio_kind(c);
    int best = -1; float bov = 0.05f;
    for (int k = 0; k < (int)clips.size(); ++k) {
        if (k == ci) continue;
        const Clip& hc = clips[(size_t)k];
        bool hostable = audio_kind
            ? (hc.clip_type == ClipType::Audio ||
               hc.clip_type == ClipType::Record ||
               hc.clip_type == ClipType::Video ||
               hc.clip_type == ClipType::VideoRecord)
            : (clip_is_videolike_type(hc.clip_type) ||
               hc.clip_type == ClipType::Background);
        if (!hostable) continue;
        float ov = fminf(c.end, hc.end) - fmaxf(c.start, hc.start);
        if (ov > bov) { bov = ov; best = k; }
    }
    if (best < 0) return ci;
    return timeline_couple_fx_brick(state, ti, ci, best);
}

// ── Synthetic mouse input (ui_input) ──────────────────────────────────────────
// Agents can't reach the canvas handles through any structured IPC method —
// transform handles are pure mouse interactions. ui_input queues scripted
// mouse steps; the main loop feeds exactly one per frame into ImGui (after
// the GLFW backend's events, so an injected position wins the frame). Used
// for agent-driven UI testing; everything runs on the main thread.
struct InputStep { float x = 0, y = 0; bool has_pos = false; int btn = -1; float wheel = 0; };  // btn: 0 down, 1 up
static std::deque<InputStep> g_input_steps;

void ipc_debug_input_tick() {
    if (g_input_steps.empty()) return;
    InputStep s = g_input_steps.front();
    g_input_steps.pop_front();
    ImGuiIO& io = ImGui::GetIO();
    if (s.has_pos)     io.AddMousePosEvent(s.x, s.y);
    if (s.btn == 0)    io.AddMouseButtonEvent(0, true);
    else if (s.btn == 1) io.AddMouseButtonEvent(0, false);
    if (s.wheel != 0.f)  io.AddMouseWheelEvent(0.f, s.wheel);
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
        // Vision models ship with the release — there is no runtime download.
        if (!std::filesystem::exists(app_models_dir() + "/moondream-decoder.onnx")) {
            err = "vision models missing — reinstall the release (models/ folder "
                  "must include the moondream files)";
            return {};
        }
        if (s_scene_analysis.running.load()) { err = "scene analysis already running"; return {}; }
        // Optional times[]: caption exactly these timestamps instead of
        // auto-detected scene changes — the text-mode contact sheet for
        // agents without vision. Capped like make_contact_sheet.
        std::vector<float> req_times;
        if (params.contains("times") && params["times"].is_array())
            for (auto& t : params["times"]) {
                req_times.push_back(t.get<float>());
                if (req_times.size() >= 48) break;
            }
        // Captioning costs ~10 s per scene — reuse a fresh sidecar instead of
        // recomputing. Custom-times runs never reuse or write the scene
        // sidecar. force=true bypasses the cache.
        if (req_times.empty() && !params.value("force", false)) {
            std::string sp = path + ".pms_scene.json";
            std::error_code ec;
            if (std::filesystem::exists(sp, ec) &&
                std::filesystem::last_write_time(sp, ec) >=
                std::filesystem::last_write_time(path, ec)) {
                s_scene_analysis.error.clear();
                s_scene_analysis.sidecar_path = sp;
                s_scene_analysis.done.store(true);
                s_scene_analysis.running.store(false);
                json r; r["status"] = "cached";
                r["hint"] = "sidecar already exists — get_video_description returns it immediately";
                return r;
            }
        }
        s_scene_analysis.running.store(true);
        s_scene_analysis.done.store(false);
        s_scene_analysis.error.clear();
        s_scene_analysis.sidecar_path.clear();
        std::thread([path, req_times]() {
            bool capped = false;
            std::vector<KeyFrame> frames = req_times.empty()
                ? extract_keyframes(path, 60, &capped)
                : extract_frames_at(path, req_times);
            if (frames.empty()) {
                s_scene_analysis.error = req_times.empty()
                    ? "keyframe extraction failed or no scene changes detected"
                    : "frame extraction failed — are the times within the video?";
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
            std::string sidecar_path = path +
                (req_times.empty() ? ".pms_scene.json" : ".pms_times.json");
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
        state.project_path = path;   // exports default next to the .pms
        recent_projects_push(path);  // surface on the home/launcher page
        recovery_clear();            // saved → no stale crash-recovery slot
        json r; r["path"] = path;
        return r;
    }

    if (method == "collect_project") {
        std::string path = params.value("path", state.project_path);
        if (path.empty()) { err = "no project path — provide 'path' param"; return {}; }
        std::string e; int copied = 0;
        if (!collect_project(state, path, e, &copied)) { err = e; return {}; }
        recent_projects_push(path);
        recovery_clear();
        json r; r["path"] = path; r["copied"] = copied;
        return r;
    }

    if (method == "seek") {
        float t = params.value("time", 0.f);
        state.playhead = t;
        audio_seek(t);   // keep the audio master clock with the playhead
        return json::object();
    }

    if (method == "ui_input") {
        // steps: [{x, y, down?, up?}] — one consumed per UI frame. A drag is
        // move → down → N moves → up. Holding a position for a frame = a step
        // with the same x/y.
        if (!params.contains("steps") || !params["steps"].is_array()) {
            err = "ui_input needs steps: [{x,y,down?,up?}]"; return {};
        }
        for (auto& st : params["steps"]) {
            InputStep s;
            if (st.contains("x") && st.contains("y")) {
                s.x = st.value("x", 0.f); s.y = st.value("y", 0.f);
                s.has_pos = true;
            }
            if (st.value("down", false))     s.btn = 0;
            else if (st.value("up", false))  s.btn = 1;
            s.wheel = st.value("wheel", 0.f);
            g_input_steps.push_back(s);
        }
        json r; r["queued"] = (int)g_input_steps.size();
        return r;
    }

    if (method == "select_clip") {
        // Set the canvas selection (the clip whose transform handles show).
        int ti = track_by_name_or_index(state, params), ci = params.value("clip", -1);
        if (ci < 0) { state.selected_track = state.selected_clip = -1; return json::object(); }
        if (!check_clip(state, ti, ci, err)) return {};
        state.selected_track = ti;
        state.selected_clip  = ci;
        return json::object();
    }

    if (method == "get_canvas_geometry") {
        // Transform-handle geometry from the last drawn frame (screen px) —
        // pairs with ui_input so agents can hit the handles deterministically.
        CanvasHandleGeom g = canvas_handle_geom();
        json r;
        r["valid"] = g.valid;
        if (g.valid) {
            r["bbox"]     = {g.bx0, g.by0, g.bx1, g.by1};
            r["rotate_knob"] = {g.rot_x, g.rot_y};
            r["center"]   = {g.cx, g.cy};
        }
        r["selected_track"] = state.selected_track;
        r["selected_clip"]  = state.selected_clip;
        if (state.selected_track >= 0 && state.selected_track < (int)state.tracks.size() &&
            state.selected_clip >= 0 &&
            state.selected_clip < (int)state.tracks[state.selected_track].clips.size()) {
            Clip& sc = state.tracks[state.selected_track].clips[state.selected_clip];
            r["rotation"] = sc.rotation;
            r["scale_x"]  = sc.scale_x;
            r["pos_x"]    = sc.pos_x;
            r["pos_y"]    = sc.pos_y;
        }
        r["pending_input_steps"] = (int)g_input_steps.size();
        return r;
    }

    if (method == "get_live_peaks") {
        // Debug: the live-recording waveform exactly as the timeline sees it.
        int n = params.value("n", 64);
        if (n < 1 || n > 4096) n = 64;
        std::vector<float> pk((size_t)n, 0.f);
        bool ok = recorder_live_peaks(n, pk.data());
        json r; r["active"] = ok;
        if (ok) r["peaks"] = pk;
        return r;
    }

    if (method == "decouple_fx_brick") {
        int ti = track_by_name_or_index(state, params), ci = params.value("clip", -1);
        if (!check_clip(state, ti, ci, err)) return {};
        if (!state.tracks[ti].clips[(size_t)ci].fx_coupled) {
            err = "clip is not a coupled FX brick"; return {};
        }
        // Lift onto a fresh track just below the content — clean movable brick.
        decouple_fx_to_new_track(state, ti, ci);
        return json::object();
    }

    if (method == "set_track_bus") {
        int ti = track_by_name_or_index(state, params);
        if (!check_track(state, ti, err)) return {};
        int b = params.value("bus", 0);
        if (b < 0 || b >= (int)state.buses.size()) { err = "bus index out of range"; return {}; }
        state.tracks[ti].bus = b;
        return json::object();
    }

    if (method == "set_bus") {
        // Create or configure a bus: {bus?: index (absent = append new),
        // name?, gain?, effects?: [{fx_type, params}] (replaces the chain)}.
        int b = params.value("bus", -1);
        if (b < 0) {
            if ((int)state.buses.size() >= MAX_BUSES) { err = "bus limit reached"; return {}; }
            Bus nb;
            char n[24];
            snprintf(n, sizeof(n), "Bus %d", (int)state.buses.size());
            nb.name = n;
            state.buses.push_back(std::move(nb));
            b = (int)state.buses.size() - 1;
        }
        if (b >= (int)state.buses.size()) { err = "bus index out of range"; return {}; }
        Bus& bus = state.buses[(size_t)b];
        if (params.contains("name")) bus.name = params.value("name", bus.name);
        if (params.contains("gain")) bus.gain = params.value("gain", bus.gain);
        if (params.contains("effects") && params["effects"].is_array()) {
            bus.fx_chain.clear();
            for (auto& e : params["effects"]) {
                std::string fxt = e.value("fx_type", "audio_reverb");
                Clip se;
                se.clip_type = ClipType::Effect;
                se.fx_type   = parse_fx_type(fxt);
                if (!fx_type_is_audio_fx(se.fx_type)) {
                    err = "'" + fxt + "' is not an audio effect"; return {};
                }
                json ap = e.value("params", json::object());
                AudioFX& afx = se.audio_fx;
                switch (se.fx_type) {
                    case FXType::AudioAutotune:
                        afx.autotune_on    = true;
                        afx.autotune_key   = ap.value("key",   afx.autotune_key);
                        afx.autotune_scale = ap.value("scale", afx.autotune_scale);
                        afx.autotune_speed = ap.value("speed_ms", afx.autotune_speed);
                        break;
                    case FXType::AudioPitch:
                        afx.pitch_on        = true;
                        afx.pitch_semitones = ap.value("semitones", afx.pitch_semitones);
                        break;
                    case FXType::AudioFormant:
                        afx.formant_on    = true;
                        afx.formant_shift = ap.value("shift", afx.formant_shift);
                        break;
                    case FXType::AudioDelay:
                        afx.delay_on       = true;
                        afx.delay_time     = ap.value("time",     afx.delay_time);
                        afx.delay_feedback = ap.value("feedback", afx.delay_feedback);
                        afx.delay_mix      = ap.value("mix",      afx.delay_mix);
                        break;
                    case FXType::AudioReverb:
                        afx.reverb_on   = true;
                        afx.reverb_room = ap.value("room", afx.reverb_room);
                        afx.reverb_damp = ap.value("damp", afx.reverb_damp);
                        afx.reverb_mix  = ap.value("mix",  afx.reverb_mix);
                        break;
                    default: break;
                }
                bus.fx_chain.push_back(se);
            }
            bus.fx_chain_selected = bus.fx_chain.empty() ? -1 : 0;
        }
        json r; r["bus"] = b;
        return r;
    }

    if (method == "get_buses") {
        json arr = json::array();
        for (int b = 0; b < (int)state.buses.size(); ++b) {
            const Bus& bus = state.buses[(size_t)b];
            json e;
            e["index"] = b;
            e["name"]  = bus.name;
            e["gain"]  = bus.gain;
            e["effects"] = (int)bus.fx_chain.size();
            json routed = json::array();
            for (int ti = 0; ti < (int)state.tracks.size(); ++ti)
                if (state.tracks[ti].bus == b) routed.push_back(ti);
            e["tracks"] = routed;
            arr.push_back(e);
        }
        json r; r["buses"] = arr;
        return r;
    }

    if (method == "get_fx_segments") {
        int ti = track_by_name_or_index(state, params), ci = params.value("clip", -1);
        if (!check_clip(state, ti, ci, err)) return {};
        auto segs = collect_audio_fx_segments(state, ti, state.tracks[ti].clips[(size_t)ci]);
        json arr = json::array();
        for (auto& sg : segs) {
            json e; e["t0"] = sg.t0; e["t1"] = sg.t1;
            e["pitch_on"] = sg.fx.pitch_on; e["semitones"] = sg.fx.pitch_semitones;
            e["reverb_on"] = sg.fx.reverb_on; e["delay_on"] = sg.fx.delay_on;
            e["autotune_on"] = sg.fx.autotune_on;
            arr.push_back(e);
        }
        json r; r["segments"] = arr;
        // Coupling diagnostics for every FX brick on the track
        json bricks = json::array();
        for (int k = 0; k < (int)state.tracks[ti].clips.size(); ++k) {
            const Clip& bc = state.tracks[ti].clips[(size_t)k];
            if (bc.clip_type != ClipType::AudioMultiFX &&
                bc.clip_type != ClipType::MultiFX) continue;
            json b;
            b["index"]   = k;
            b["type"]    = clip_type_str(bc.clip_type);
            b["coupled"] = bc.fx_coupled;
            b["host_sid"] = bc.fx_host_sid;
            b["resolved_host"] = fx_coupled_host(state, ti, bc);
            json ents = json::array();
            for (auto& se : bc.fx_chain) {
                json e; e["fx_type"] = (int)se.fx_type;
                e["pitch_on"] = se.audio_fx.pitch_on;
                e["rel0"] = se.rel_start; e["rel1"] = se.rel_end;
                ents.push_back(e);
            }
            b["entries"] = ents;
            bricks.push_back(b);
        }
        r["bricks"] = bricks;
        return r;
    }

    if (method == "dump_face_input") {
        bool ok = face_track_dump_last("/tmp/face_input.ppm");
        json r; r["ok"] = ok;
        return r;
    }

    if (method == "get_face_track") {
        FaceObs obs;
        bool ok = face_track_latest(obs);
        json r;
        r["available"] = face_track_available();
        r["valid"]     = ok;
        {
            extern std::atomic<int> g_dbg_flip180, g_dbg_since_detect, g_dbg_detects;
            r["flip180"]      = g_dbg_flip180.load();
            r["since_detect"] = g_dbg_since_detect.load();
            r["detects"]      = g_dbg_detects.load();
        }
        if (ok) {
            r["score"] = obs.score;
            r["frame"] = {obs.w, obs.h};
            r["nose"]  = {obs.pts[80][0], obs.pts[80][1]};
            r["chin"]  = {obs.pts[0][0], obs.pts[0][1]};
            r["eyeB"]  = {obs.pts[90][0], obs.pts[90][1]};
            if (params.value("full", false)) {
                json pts = json::array();
                for (int k = 0; k < 106; ++k)
                    pts.push_back({obs.pts[k][0], obs.pts[k][1]});
                r["pts"] = pts;
            }
        }
        // Mirror overlay debug: quad geometry + raw-frame landmarks exactly
        // as the doggy overlay consumes them (post rotation remap).
        MirrorDebugGeom mg = mirror_debug_geom();
        if (mg.valid) {
            json m;
            m["center"]  = {mg.cx, mg.cy};
            m["half"]    = {mg.hw, mg.hh};
            m["rot_deg"] = mg.rot_deg;
            m["cam"]     = {mg.cam_w, mg.cam_h};
            m["face_valid"] = mg.face_valid;
            if (mg.face_valid && params.value("full", false)) {
                json pts = json::array();
                for (int k = 0; k < 106; ++k)
                    pts.push_back({mg.pts[k][0], mg.pts[k][1]});
                m["pts"] = pts;
            }
            r["mirror"] = m;
        }
        return r;
    }

    if (method == "debug_fx_tone") {
        // DSP self-test: 2 s of 440 Hz through process_audio_fx with the
        // given params; returns Goertzel powers so divergence between the
        // collection layer and the DSP layer is attributable in one call.
        AudioFX fx;
        fx.pitch_on        = true;
        fx.pitch_semitones = params.value("semitones", 12.f);
        const int sr = 44100, n = sr * 2;
        std::vector<float> raw((size_t)n * 2);
        for (int i = 0; i < n; ++i) {
            float v = 0.5f * sinf(2.f * 3.14159265f * 440.f * (float)i / sr);
            raw[(size_t)i*2] = raw[(size_t)i*2+1] = v;
        }
        std::vector<float> proc = process_audio_fx(raw, fx, (float)sr);
        auto goertzel = [&](const std::vector<float>& b, float freq) {
            double k = 2.0 * 3.14159265358979 * freq / sr, s0, s1 = 0, s2 = 0;
            for (size_t i = b.size()/2; i + 1 < b.size(); i += 2) {
                s0 = b[i] + 2.0*cos(k)*s1 - s2; s2 = s1; s1 = s0;
            }
            return s1*s1 + s2*s2 - 2.0*cos(k)*s1*s2;
        };
        json r;
        r["in_440"]   = goertzel(raw, 440.f);
        r["out_440"]  = goertzel(proc, 440.f);
        r["out_880"]  = goertzel(proc, 880.f);
        return r;
    }

    if (method == "set_monitor") {
        // Live mic monitoring controls — parity with the record panel's
        // "Hear yourself" / "Hear effects" / gate toggles.
        bool on = params.value("on", true);
        if (on && !audio_capture_start()) { err = "mic open failed"; return {}; }
        audio_monitor_set(on);
        if (params.contains("hear_fx"))
            audio_monitor_fx_set(params.value("hear_fx", false));
        if (params.contains("gate"))
            audio_gate_set(params.value("gate", true));
        if (!on && !recorder_active()) audio_capture_stop();
        return json::object();
    }

    if (method == "get_audio_perf") {
        // Low-latency duplex (performance mode) health — see audio.h.
        json r;
        r["perf_mode"]     = audio_perf_mode();
        r["xruns"]         = audio_perf_xruns();
        r["out_latency_s"] = audio_latency();
        r["in_latency_s"]  = audio_capture_latency();
        r["monitor"]       = audio_monitor_get();
        r["gate"]          = audio_gate_get();
        r["hear_fx"]       = audio_monitor_fx_get();
        r["chain_active"]  = audio_monitor_chain_active();
        return r;
    }

    if (method == "set_camera_monitor") {
        bool on = params.value("on", true);
        vrecorder_monitor_set(on);
        json r; r["monitor"] = vrecorder_monitor_get();
        return r;
    }

    if (method == "vrecord_start") {
        int ti = params.value("track", -1);
        int ci = params.value("clip", -1);
        if (!vrecorder_start(state, ti, ci)) { err = "vrecord_start failed (not a camera brick / too short / capture failed)"; return {}; }
        json r; r["status"] = "recording";
        r["test_pattern"] = vrecorder_using_test_pattern();
        return r;
    }
    if (method == "vrecord_stop") {
        vrecorder_stop(state);
        json r; r["takes"] = vrecorder_take_count();
        return r;
    }

    if (method == "play") {
        // Mirror toggle_play: without audio_play() the audio engine never
        // started for agent-driven playback (playhead ran on the wall-clock
        // fallback, output stream stayed corked).
        if (!state.playing) {
            state.playing = true;
            state.play_start_wall = std::chrono::steady_clock::now();
            state.play_start_pos  = state.playhead;
            audio_seek(state.playhead);
            audio_play();
        }
        return json::object();
    }

    if (method == "pause") {
        state.playing = false;
        audio_pause();
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

    // ── In-app agent debug surface (AGENT_HARNESS.md) ────────────────────────
    // Not exposed as MCP tools; used to drive/inspect the embedded agent
    // headlessly (tests, debugging). agent_send opens the panel so the
    // activity is visible in the UI.
    if (method == "agent_send") {
        std::string text = params.value("text", "");
        if (text.empty()) { err = "text required"; return {}; }
        if (agent_running()) { err = "agent is already running a turn"; return {}; }
        state.agent_panel_open = true;
        state.terminal_open    = false;  // bottom strip is single-occupancy
        agent_send(text);
        return json::object();
    }

    if (method == "agent_chat_state") {
        json rows = json::array();
        for (auto& r : agent_rows_snapshot()) {
            const char* role =
                r.role == AgentRole::User      ? "user"      :
                r.role == AgentRole::Assistant ? "assistant" :
                r.role == AgentRole::Tool      ? "tool"      :
                r.role == AgentRole::Error     ? "error"     :
                r.role == AgentRole::Image     ? "image"     : "info";
            rows.push_back({{"role", role}, {"text", r.text},
                            {"streaming", r.streaming}});
        }
        return json{{"running", agent_running()}, {"rows", rows}};
    }

    if (method == "take_snapshot") {
        if (params.contains("time"))
            state.playhead = params["time"].get<float>();
        state.snapshot_done      = false;
        state.snapshot_done_path.clear();
        state.snapshot_done_err.clear();
        // "ui" rides the canvas-capture path but grabs the whole window
        // backbuffer — timeline, panels, everything the user is looking at.
        std::string snap_src = params.value("source", "render");
        state.snapshot_source_canvas = (snap_src == "canvas" || snap_src == "ui");
        state.snapshot_source_ui     = (snap_src == "ui");
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
        // Camera bricks count: a selected take mirrors its path into `text`,
        // so the mask pipeline reads it like any video clip.
        if (!clip_is_videolike_type(cl_pre.clip_type) || cl_pre.text.empty()) {
            err = "clip is not a video clip (or camera brick with a take)"; return {};
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
        if (clip_is_fx(cl) &&
            fx_overlap_on_track(state, ti, start, start + dur, ci, err))
            return {};
        cl.start = start;
        cl.end   = start + dur;
        return json::object();
    }

    if (method == "trim_clip") {
        int ti = track_by_name_or_index(state, params), ci = params.value("clip", -1);
        if (!check_clip(state, ti, ci, err)) return {};
        Clip& cl = state.tracks[ti].clips[ci];
        if (clip_is_fx(cl)) {
            float ns = params.contains("start")
                ? snap_to_frame(params["start"].get<float>(), state.fps) : cl.start;
            float ne = params.contains("end")
                ? snap_end_to_frame(params["end"].get<float>(), state.fps) : cl.end;
            if (fx_overlap_on_track(state, ti, ns, ne, ci, err)) return {};
        }
        float old_start = cl.start;
        if (params.contains("start")) {
            float ns = snap_to_frame(params["start"].get<float>(), state.fps);
            float delta = ns - old_start;
            // in_point is in source seconds — scale the timeline delta by
            // speed (this used to skip that, desyncing trims of retimed
            // clips), and shift keyframes so they stay put on the timeline.
            cl.in_point = fmaxf(0.f, cl.in_point + delta * fmaxf(0.01f, cl.speed));
            clip_keys_shift(cl, -delta);
            cl.start = ns;
        }
        if (params.contains("end")) cl.end = snap_end_to_frame(params["end"].get<float>(), state.fps);
        return json::object();
    }

    if (method == "split_clip") {
        int ti = track_by_name_or_index(state, params), ci = params.value("clip", -1);
        if (!check_clip(state, ti, ci, err)) return {};

        // Scalar 'time' or array 'times' — agents making a multi-cut edit get
        // all the cuts in one call instead of burning a model round per cut.
        std::vector<float> times;
        if (params.contains("times") && params["times"].is_array())
            for (auto& v : params["times"])
                if (v.is_number()) times.push_back(v.get<float>());
        if (params.contains("time") && params["time"].is_number())
            times.push_back(params["time"].get<float>());
        if (times.empty()) { err = "missing 'time' or 'times'"; return {}; }
        std::sort(times.begin(), times.end());
        times.erase(std::unique(times.begin(), times.end(),
                                [](float a, float b) { return fabsf(a - b) < 1e-4f; }),
                    times.end());
        // Validate everything against the original bounds before touching the
        // track, so a bad entry can't leave a half-applied multi-split.
        {
            const Clip& cl = state.tracks[ti].clips[ci];
            for (float t : times)
                if (t <= cl.start || t >= cl.end) {
                    char buf[96];
                    snprintf(buf, sizeof(buf),
                             "split time %.3f outside clip range %.3f-%.3f",
                             t, cl.start, cl.end);
                    err = buf;
                    return {};
                }
        }

        // clip_split_at also scales in_point by speed (this path used to forget
        // that, desyncing splits of retimed clips) and remaps keyframe tracks.
        // Split right-to-left so the remaining cut points stay inside clip ci.
        for (int k = (int)times.size() - 1; k >= 0; --k) {
            Clip right = clip_split_at(state.tracks[ti].clips[ci], times[k]);
            state.tracks[ti].clips.insert(
                state.tracks[ti].clips.begin() + ci + 1, std::move(right));
        }
        json r;
        r["left_clip"]  = ci;
        r["right_clip"] = ci + 1;
        if (times.size() > 1) {
            json idx = json::array();
            for (size_t k = 0; k <= times.size(); ++k) idx.push_back(ci + (int)k);
            r["clips"] = idx;  // every piece, leftmost first
        }
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
        if (!apply_bg_preset(cl, text, err)) return {};
        if (clip_is_fx(cl) && fx_overlap_on_track(state, ti, start, end, -1, err))
            return {};
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
            if (!apply_bg_preset(cl, text, err)) return {};
            if (clip_is_fx(cl) && fx_overlap_on_track(state, ti, start, end, -1, err))
                return {};
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
            else if (prop == "speed")     {
                // Match the UI speed control: retime the clip's timeline width
                // (and overlapping FX bricks) so the same source span plays at
                // the new speed. Without this a 100x timelapse kept its old
                // width and read source minutes past EOF — invisible in the UI
                // and lethal to exports. retime=false keeps start/end and
                // changes which source range plays instead.
                float ns = fmaxf(0.25f, fminf(100.f, jval_float(val)));
                float ratio = ns / fmaxf(0.01f, cl.speed);
                cl.speed = ns;
                if ((cl.clip_type == ClipType::Video || cl.clip_type == ClipType::Audio) &&
                    op.value("retime", true))
                    rescale_glass_bricks(state, ti, ci, ratio);
            }
            else if (prop == "opacity")   { cl.opacity   = jval_float(val); }
            else if (prop == "muted")     { cl.muted     = jval_bool(val); }
            else if (prop == "in_point")  { cl.in_point  = jval_float(val); }
            else if (prop == "fade_in")   { cl.fade_in   = jval_float(val); }
            else if (prop == "fade_out")  { cl.fade_out  = jval_float(val); }
            else if (prop == "blend_mode"){ cl.blend_mode= jval_int(val); }
            else if (prop == "selected_take") {
                cl.rec_take_sel = std::clamp(jval_int(val), -1,
                                             (int)cl.rec_takes.size() - 1);
            }
            else if (prop == "pos_x")     { cl.pos_x     = jval_float(val); }
            else if (prop == "pos_y")     { cl.pos_y     = jval_float(val); }
            else if (prop == "scale_x")   { cl.scale_x   = jval_float(val); }
            else if (prop == "scale_y")   { cl.scale_y   = jval_float(val); }
            else if (prop == "rotation")  { cl.rotation  = jval_float(val); }
            else if (prop == "face_filter")     { cl.face_filter = (int)jval_float(val); }
            else if (prop == "face_filter_amt") { cl.face_filter_amt = jval_float(val); }
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
            else if (prop == "bg_speed")     { cl.bg_speed     = jval_float(val); }
            else if (prop == "bg_intensity") { cl.bg_intensity = jval_float(val); }
            else if (prop == "bg_c1" || prop == "bg_c2" || prop == "bg_c3") {
                if (!val.is_array() || val.size() != 4) { err = prop + " must be [r,g,b,a]"; return {}; }
                float* dst = prop == "bg_c1" ? cl.bg_c1
                           : prop == "bg_c2" ? cl.bg_c2 : cl.bg_c3;
                for (int i = 0; i < 4; ++i) dst[i] = jval_float(val[i]);
            }
            else { err = "unknown prop: " + prop; return {}; }
        }
        return json::object();
    }

    if (method == "set_clip_keyframes") {
        int ti = track_by_name_or_index(state, params), ci = params.value("clip", -1);
        std::string prop = params.value("prop", "");
        if (!check_clip(state, ti, ci, err)) return {};
        bool ok_prop = (prop == "opacity");          // opacity is special-cased in eval_prop
        for (int i = 0; !ok_prop && i < kClipKfFieldCount; ++i)
            if (prop == kClipKfFields[i].name) ok_prop = true;
        if (!ok_prop) {
            err = "prop '" + prop + "' is not keyframable";
            return {};
        }
        if (!params.contains("keys") || !params["keys"].is_array()) {
            err = "keys must be an array of {t, v, interp?} (empty array clears)";
            return {};
        }
        Clip& cl = state.tracks[ti].clips[ci];
        PropTrack pt;
        for (auto& k : params["keys"]) {
            if (!k.contains("t") || !k.contains("v")) { err = "each key needs t and v"; return {}; }
            float t = k["t"].get<float>();
            if (t < 0.f || t > cl.end - cl.start + 1e-3f) {
                err = "key t=" + std::to_string(t) + " outside clip duration (t is "
                      "seconds relative to clip start)";
                return {};
            }
            pt.set(t, k["v"].get<float>(),
                   interp_from_str(k.value("interp", "ease_both")));
        }
        if (pt.empty()) cl.ktracks.erase(prop);
        else            cl.ktracks[prop] = std::move(pt);
        json r;
        r["prop"] = prop;
        r["key_count"] = cl.ktracks.count(prop) ? (int)cl.ktracks[prop].keys.size() : 0;
        return r;
    }

    if (method == "set_clip_prop") {
        int ti = track_by_name_or_index(state, params), ci = params.value("clip", -1);
        std::string prop = params.value("prop", "");
        if (!check_clip(state, ti, ci, err)) return {};
        Clip& cl = state.tracks[ti].clips[ci];

        auto& val = params["value"];
        // ── A/V props ────────────────────────────────────────────────────────
        if      (prop == "volume")   { cl.volume     = jval_float(val); }
        else if (prop == "speed")    {
            // Same retime semantics as the UI speed control — see set_clip_props.
            float ns = fmaxf(0.25f, fminf(100.f, jval_float(val)));
            float ratio = ns / fmaxf(0.01f, cl.speed);
            cl.speed = ns;
            if ((cl.clip_type == ClipType::Video || cl.clip_type == ClipType::Audio) &&
                params.value("retime", true))
                rescale_glass_bricks(state, ti, ci, ratio);
        }
        else if (prop == "opacity")  { cl.opacity    = jval_float(val); }
        else if (prop == "muted")    { cl.muted      = jval_bool(val); }
        else if (prop == "in_point") { cl.in_point   = jval_float(val); }
        else if (prop == "fade_in")  { cl.fade_in    = jval_float(val); }
        else if (prop == "fade_out") { cl.fade_out   = jval_float(val); }
        else if (prop == "blend_mode") { cl.blend_mode = jval_int(val); }
        else if (prop == "selected_take") {
            cl.rec_take_sel = std::clamp(jval_int(val), -1,
                                         (int)cl.rec_takes.size() - 1);
        }
        // ── Transform props ──────────────────────────────────────────────────
        else if (prop == "pos_x")    { cl.pos_x    = jval_float(val); }
        else if (prop == "pos_y")    { cl.pos_y    = jval_float(val); }
        else if (prop == "scale_x")  { cl.scale_x  = jval_float(val); }
        else if (prop == "scale_y")  { cl.scale_y  = jval_float(val); }
        else if (prop == "rotation") { cl.rotation = jval_float(val); }
        else if (prop == "face_filter")     { cl.face_filter = (int)jval_float(val); }
        else if (prop == "face_filter_amt") { cl.face_filter_amt = jval_float(val); }
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
        // ── Background props (ClipType::Background) ──────────────────────────
        else if (prop == "bg_speed")     { cl.bg_speed     = jval_float(val); }
        else if (prop == "bg_intensity") { cl.bg_intensity = jval_float(val); }
        else if (prop == "bg_c1" || prop == "bg_c2" || prop == "bg_c3") {
            if (!val.is_array() || val.size() != 4) { err = prop + " must be [r,g,b,a]"; return {}; }
            float* dst = prop == "bg_c1" ? cl.bg_c1
                       : prop == "bg_c2" ? cl.bg_c2 : cl.bg_c3;
            for (int i = 0; i < 4; ++i) dst[i] = jval_float(val[i]);
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
        int ti = track_by_name_or_index(state, params);
        if (!check_track(state, ti, err)) return {};
        state.tracks.erase(state.tracks.begin() + ti);
        if (state.selected_track == ti) { state.selected_track = -1; state.selected_clip = -1; }
        return json::object();
    }

    if (method == "add_marker") {
        // Markers live on the frame grid like the playhead — never mid-frame.
        float mtime  = snap_to_frame(state, fmaxf(0.f, params.value("time", 0.f)));
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

    if (method == "set_loop_region") {
        // Arm/disarm the transport loop and optionally set the brace region.
        // Omit start/end (or pass end<=start) to loop the whole timeline.
        state.loop_play = params.value("enabled", true);
        if (params.contains("start") && params.contains("end")) {
            // Snap the brace to the frame grid so the loop never wraps mid-frame.
            float s = snap_to_frame(state, params.value("start", 0.f));
            float e = snap_to_frame(state, params.value("end", 0.f));
            if (e > s) { state.loop_in = s; state.loop_out = e; }
            else       { state.loop_in = -1.f; state.loop_out = -1.f; }
        } else {
            state.loop_in = -1.f; state.loop_out = -1.f;
        }
        json r;
        r["loop_play"] = state.loop_play;
        r["loop_in"]   = state.loop_in;
        r["loop_out"]  = state.loop_out;
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
        state.project_path   = path;   // exports default next to the .pms
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

        // Audio FX bricks: enable the effect and take its params directly.
        if (fx_type_is_audio_fx(cl.fx_type)) {
            json ap = params.value("params", json::object());
            AudioFX& afx = cl.audio_fx;
            switch (cl.fx_type) {
                case FXType::AudioAutotune:
                    afx.autotune_on    = true;
                    afx.autotune_key   = ap.value("key",   afx.autotune_key);
                    afx.autotune_scale = ap.value("scale", afx.autotune_scale);
                    afx.autotune_speed = ap.value("speed_ms", afx.autotune_speed);
                    break;
                case FXType::AudioPitch:
                    afx.pitch_on        = true;
                    afx.pitch_semitones = ap.value("semitones", afx.pitch_semitones);
                    break;
                case FXType::AudioFormant:
                    afx.formant_on    = true;
                    afx.formant_shift = ap.value("shift", afx.formant_shift);
                    break;
                case FXType::AudioDelay:
                    afx.delay_on       = true;
                    afx.delay_time     = ap.value("time",     afx.delay_time);
                    afx.delay_feedback = ap.value("feedback", afx.delay_feedback);
                    afx.delay_mix      = ap.value("mix",      afx.delay_mix);
                    break;
                case FXType::AudioReverb:
                    afx.reverb_on   = true;
                    afx.reverb_room = ap.value("room", afx.reverb_room);
                    afx.reverb_damp = ap.value("damp", afx.reverb_damp);
                    afx.reverb_mix  = ap.value("mix",  afx.reverb_mix);
                    break;
                default: break;
            }
        } else if (params.contains("params") && params["params"].is_object()) {
            if (!apply_effect_params(cl, params["params"], fx_s, err)) return {};
        }

        if (fx_overlap_on_track(state, ti, start, end, -1, err)) return {};
        state.tracks[ti].clips.push_back(cl);
        int nci = ipc_autocouple_fx(state, ti, (int)state.tracks[ti].clips.size() - 1);
        json r; r["clip"] = nci;
        r["coupled"] = state.tracks[ti].clips[(size_t)nci].fx_coupled;
        return r;
    }

    if (method == "add_audio_multifx_brick") {
        // Audio chain brick: effects = [{fx_type: audio_*, params, rel_start?,
        // rel_end?}]. Auto-couples to the best-overlap audio content.
        int ti = track_by_name_or_index(state, params);
        if (!check_track(state, ti, err)) return {};
        if (state.tracks[ti].locked) { err = "track is locked"; return {}; }
        float start = snap_to_frame(params.value("start", 0.f), state.fps);
        float end   = snap_end_to_frame(params.value("end", start + 2.f), state.fps);

        Clip brick;
        brick.clip_type = ClipType::AudioMultiFX;
        brick.start     = start;
        brick.end       = end;
        if (params.contains("effects") && params["effects"].is_array()) {
            for (auto& e : params["effects"]) {
                std::string fxt = e.value("fx_type", "audio_autotune");
                Clip se;
                se.clip_type = ClipType::Effect;
                se.fx_type   = parse_fx_type(fxt);
                if (!fx_type_is_audio_fx(se.fx_type)) {
                    err = "'" + fxt + "' is not an audio effect — the Audio "
                          "Multi-FX brick takes audio effects only";
                    return {};
                }
                se.rel_start = e.value("rel_start", 0.f);
                se.rel_end   = e.value("rel_end",   0.f);
                json ap = e.value("params", json::object());
                AudioFX& afx = se.audio_fx;
                switch (se.fx_type) {
                    case FXType::AudioAutotune:
                        afx.autotune_on    = true;
                        afx.autotune_key   = ap.value("key",   afx.autotune_key);
                        afx.autotune_scale = ap.value("scale", afx.autotune_scale);
                        afx.autotune_speed = ap.value("speed_ms", afx.autotune_speed);
                        break;
                    case FXType::AudioPitch:
                        afx.pitch_on        = true;
                        afx.pitch_semitones = ap.value("semitones", afx.pitch_semitones);
                        break;
                    case FXType::AudioFormant:
                        afx.formant_on    = true;
                        afx.formant_shift = ap.value("shift", afx.formant_shift);
                        break;
                    case FXType::AudioDelay:
                        afx.delay_on       = true;
                        afx.delay_time     = ap.value("time",     afx.delay_time);
                        afx.delay_feedback = ap.value("feedback", afx.delay_feedback);
                        afx.delay_mix      = ap.value("mix",      afx.delay_mix);
                        break;
                    case FXType::AudioReverb:
                        afx.reverb_on   = true;
                        afx.reverb_room = ap.value("room", afx.reverb_room);
                        afx.reverb_damp = ap.value("damp", afx.reverb_damp);
                        afx.reverb_mix  = ap.value("mix",  afx.reverb_mix);
                        break;
                    default: break;
                }
                brick.fx_chain.push_back(se);
            }
        }
        brick.fx_chain_selected = brick.fx_chain.empty() ? -1 : 0;
        if (fx_overlap_on_track(state, ti, start, end, -1, err)) return {};
        state.tracks[ti].clips.push_back(brick);
        int nci = ipc_autocouple_fx(state, ti, (int)state.tracks[ti].clips.size() - 1);
        json r; r["clip"] = nci;
        r["coupled"] = state.tracks[ti].clips[(size_t)nci].fx_coupled;
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
                    if (fx_type_is_audio_fx(se.fx_type)) {
                        err = "'" + fxt + "' is an audio effect — the Video Multi-FX "
                              "brick takes video effects only (audio gets its own "
                              "chain brick)";
                        return {};
                    }
                    if (e.contains("params") && e["params"].is_object()) {
                        if (!apply_effect_params(se, e["params"], fxt, err)) return {};
                    }
                }
                brick.fx_chain.push_back(se);
            }
        }

        if (fx_overlap_on_track(state, ti, start, end, -1, err)) return {};
        state.tracks[ti].clips.push_back(brick);
        int nci = ipc_autocouple_fx(state, ti, (int)state.tracks[ti].clips.size() - 1);
        json r; r["clip"] = nci;
        r["coupled"] = state.tracks[ti].clips[(size_t)nci].fx_coupled;
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
                    if (err.empty()) {
                        // quiet: keep the handler's own payload (plus an ok
                        // ack) instead of replacing it with the full project
                        // dump — verbose mutation responses cost agents
                        // thousands of tokens per call, and the replacement
                        // also swallowed useful returns like add_clip's
                        // {"clip": n}.
                        // Tolerant parse: clients sometimes send "true" as a
                        // string; value() would throw a type error on that.
                        bool quiet = false;
                        if (params.contains("quiet")) {
                            const auto& q = params["quiet"];
                            quiet = q.is_boolean() ? q.get<bool>()
                                  : (q.is_string() && q.get<std::string>() == "true");
                        }
                        if (quiet) {
                            if (!result.is_object()) result = json::object();
                            result["ok"] = true;
                        } else {
                            result = state_to_json(state);
                        }
                    }
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
