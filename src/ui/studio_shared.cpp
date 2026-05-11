#include "studio_types.h"
#include "studio_shared.h"
#include "app.h"
#include "audio.h"
#include "video.h"
#include "proxy.h"
#include "history.h"
#include "fx_shader.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <filesystem>
#include <algorithm>
#include <set>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <chrono>

namespace fs = std::filesystem;

extern double s_scrub_until;  // owned by canvas.cpp (used in ui_studio scrub expiry)

// ── Shared source-duration cache ──────────────────────────────────────────────
std::unordered_map<std::string, float> s_source_durations;

// ── Time formatting ───────────────────────────────────────────────────────────
std::string fmt_time(float s) {
    int m  = (int)(s / 60);
    int sc = (int)s % 60;
    int cs = (int)((s - floorf(s)) * 100.f);
    char buf[20]; snprintf(buf, sizeof(buf), "%d:%02d.%02d", m, sc, cs);
    return buf;
}

std::string fmt_time_short(float s) {
    int m  = (int)(s / 60);
    int sc = (int)s % 60;
    char buf[12]; snprintf(buf, sizeof(buf), "%d:%02d", m, sc);
    return buf;
}

// ── Playback helpers ──────────────────────────────────────────────────────────
void seek_to(AppState& state, float t) {
    t = roundf(t * 30.f) / 30.f;
    state.playhead = t;
    audio_seek(t);
    if (state.playing) {
        state.play_start_pos  = t;
        state.play_start_wall = std::chrono::steady_clock::now();
    } else {
        audio_play();
        s_scrub_until = ImGui::GetTime() + 0.08;
    }
}

void toggle_play(AppState& state) {
    state.playing = !state.playing;
    if (state.playing) {
        state.play_start_pos  = state.playhead;
        state.play_start_wall = std::chrono::steady_clock::now();
        audio_seek(state.playhead);
        audio_play();
    } else {
        audio_pause();
    }
}

float tl_fps(const AppState& state) {
    return (state.proxy_ready && video_info(0).fps > 0.0)
           ? (float)video_info(0).fps : (float)state.fps;
}

// ── Clip / slot helpers ───────────────────────────────────────────────────────
std::string clip_slot_key(const std::string& src, float start) {
    char buf[32]; snprintf(buf, sizeof(buf), "\x01%.6f", start);
    return src + buf;
}

std::string source_from_key(const std::string& key) {
    auto pos = key.find('\x01');
    return pos == std::string::npos ? key : key.substr(0, pos);
}

int slot_for_video(AppState& state, const std::string& key, const std::string& /*src*/) {
    if (key.empty()) return -1;
    for (int i = 0; i < MAX_VIDEO_TRACKS; ++i)
        if (state.proxy_paths[i] == key) return i;
    for (int i = 0; i < MAX_VIDEO_TRACKS; ++i)
        if (state.proxy_paths[i].empty()) { state.proxy_paths[i] = key; return i; }
    return -1;
}

void gc_video_slots(AppState& state) {
    std::set<std::string> live;
    for (auto& tr : state.tracks)
        for (auto& cl : tr.clips)
            if (cl.clip_type == ClipType::Video && !cl.text.empty())
                live.insert(clip_slot_key(cl.text, cl.start));
    for (int i = 0; i < MAX_VIDEO_TRACKS; ++i) {
        if (!state.proxy_paths[i].empty() && !live.count(state.proxy_paths[i])) {
            video_close(i);
            state.proxy_paths[i].clear();
        }
    }
}

void reopen_video_slots(AppState& state) {
    for (auto& tr : state.tracks) {
        for (auto& cl : tr.clips) {
            if (cl.clip_type != ClipType::Video || cl.text.empty()) continue;
            std::string key = clip_slot_key(cl.text, cl.start);
            int slot = slot_for_video(state, key, cl.text);
            if (slot < 0) continue;
            proxy_start(cl.text);
            video_open_still(slot, proxy_still_path(cl.text));
            if (proxy_is_ready(cl.text)) {
                ProxyInfo pi;
                if (proxy_load(cl.text, pi)) {
                    video_open_proxy(slot, pi);
                    if (slot == 0) state.proxy_ready = true;
                }
            }
        }
    }
}

float project_end(const AppState& state) {
    float end = 0.f;
    for (auto& tr : state.tracks)
        for (auto& cl : tr.clips)
            if (cl.end > end) end = cl.end;
    return fmaxf(end, 0.01f);
}

bool is_audio_file(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    for (auto& c : ext) c = (char)tolower((unsigned char)c);
    return ext==".wav"||ext==".mp3"||ext==".m4a"||
           ext==".flac"||ext==".mp4"||ext==".mov"||ext==".aac";
}

void add_clip_to_track(AppState& state, int ti, const std::string& path, ClipType ct) {
    if (ti < 0 || ti >= (int)state.tracks.size()) return;
    Track& tr = state.tracks[ti];

    Clip cl;
    cl.clip_type = ct;
    cl.start     = state.playhead;
    cl.text      = path;
    cl.source_id = path;

    if (ct == ClipType::Video) {
        float dur = video_probe_duration(path);
        if (dur <= 0.f) dur = 4.f;
        cl.end = cl.start + dur;
        s_source_durations[path] = dur;
        int slot = slot_for_video(state, clip_slot_key(path, cl.start), path);
        proxy_start(path);
        if (slot >= 0) {
            video_open_still(slot, proxy_still_path(path));
            if (proxy_is_ready(path)) {
                ProxyInfo pi;
                if (proxy_load(path, pi)) video_open_proxy(slot, pi);
            }
        }
        state.video_loaded = true;
    } else if (ct == ClipType::Audio) {
        AudioMeta meta;
        float dur = audio_probe(path, meta) ? meta.duration_secs : 0.f;
        if (dur <= 0.f) dur = 4.f;
        cl.end = cl.start + dur;
        s_source_durations[path] = dur;
        audio_source_ensure(path);
    } else {
        cl.end = cl.start + 2.f;
    }

    tr.clips.push_back(cl);
    std::sort(tr.clips.begin(), tr.clips.end(),
              [](const Clip& a, const Clip& b){ return a.start < b.start; });

    // Ask draw_timeline to zoom out if the clip extends past the visible right edge.
    // Deferred so it always runs with a valid clip_area_w even on the very first frame.
    state.tl_zoom_to_fit_end = cl.end;

    state.selected_track = ti;
    for (int ci = 0; ci < (int)tr.clips.size(); ++ci)
        if (&tr.clips[ci] == &tr.clips.back() ||
            (fabsf(tr.clips[ci].start - cl.start) < 0.01f &&
             tr.clips[ci].clip_type == ct))
            { state.selected_clip = ci; break; }
    s_panel_view = PanelView::Clip;
    history_push(state, std::string("Add ") +
                        (ct==ClipType::Video ? "video" :
                         ct==ClipType::Audio ? "audio" : "text") + " clip");
}

// ── Panel-view helpers ────────────────────────────────────────────────────────
bool pv_is_lib(PanelView v) {
    return v == PanelView::LibBG  || v == PanelView::LibFX  || v == PanelView::LibAdj ||
           v == PanelView::LibVID || v == PanelView::LibIMG || v == PanelView::LibAUD;
}

bool pv_is_override(PanelView v) {
    return v == PanelView::OverrideFX || v == PanelView::OverrideAdj || v == PanelView::OverrideBG;
}

PanelView pv_derive(const AppState& state) {
    bool hs = state.selected_track >= 0 &&
              state.selected_track < (int)state.tracks.size() &&
              state.selected_clip  >= 0 &&
              state.selected_clip  < (int)state.tracks[state.selected_track].clips.size();
    if (!hs) return PanelView::Project;
    const Clip& cl = state.tracks[state.selected_track].clips[state.selected_clip];
    if (cl.clip_type == ClipType::Background) return PanelView::OverrideBG;
    if (cl.clip_type == ClipType::Effect) {
        if (cl.fx_type == FXType::Grade    ||
            cl.fx_type == FXType::Blur     ||
            cl.fx_type == FXType::Vignette)
            return PanelView::OverrideAdj;
        return PanelView::OverrideFX;
    }
    if (cl.clip_type == ClipType::Text || cl.clip_type == ClipType::Lyrics ||
        cl.clip_type == ClipType::Subtitle) return PanelView::Typography;
    return PanelView::Clip;
}

// ── FX / clip display helpers ─────────────────────────────────────────────────
ImVec4 clip_type_badge_color(ClipType ct) {
    switch (ct) {
        case ClipType::Text:       return {80.f/255,140.f/255,220.f/255,1.f};
        case ClipType::Lyrics:     return {220.f/255,160.f/255,40.f/255,1.f};
        case ClipType::Subtitle:   return {40.f/255,190.f/255,190.f/255,1.f};
        case ClipType::Video:      return {140.f/255,60.f/255,220.f/255,1.f};
        case ClipType::Audio:      return {50.f/255,180.f/255,100.f/255,1.f};
        case ClipType::Background: return {180.f/255,60.f/255,160.f/255,1.f};
        default:                   return {120.f/255,80.f/255,220.f/255,1.f};
    }
}

const char* clip_type_name(ClipType ct) {
    switch (ct) {
        case ClipType::Text:       return "TEXT";
        case ClipType::Lyrics:     return "LYRICS";
        case ClipType::Subtitle:   return "SUBTITLE";
        case ClipType::Video:      return "VIDEO";
        case ClipType::Audio:      return "AUDIO";
        case ClipType::Background: return "BG";
        default:                   return "ADJUST";
    }
}

ImU32 fx_type_accent(FXType ft) {
    switch (ft) {
        case FXType::Glitch:     return IM_COL32(0,210,220,255);
        case FXType::ZoomPunch:  return IM_COL32(255,135,40,255);
        case FXType::LUT:        return IM_COL32(255,205,55,255);
        case FXType::LightLeak:  return IM_COL32(255,90,160,255);
        case FXType::VHS:        return IM_COL32(110,195,95,255);
        case FXType::Datamosh:   return IM_COL32(255,60,100,255);
        case FXType::ChromaKey:  return IM_COL32(50,220,120,255);
#include "generated/fx_ui_color.h"
        default:                return IM_COL32(120,80,220,255);
    }
}

const char* fx_type_name(FXType ft) {
    switch (ft) {
        case FXType::Glitch:    return "GLITCH";
        case FXType::ZoomPunch: return "ZOOM";
        case FXType::LUT:       return "LUT";
        case FXType::LightLeak: return "LEAK";
        case FXType::VHS:       return "VHS";
        case FXType::Datamosh:  return "MOSH";
        case FXType::ChromaKey: return "KEY";
        case FXType::Grade:     return "Grade";
        case FXType::Blur:      return "Blur";
        case FXType::Vignette:  return "Vignette";
#include "generated/fx_ui_abbrev.h"
        default:                return "FX";
    }
}

const char* fx_type_display(FXType ft) {
    switch (ft) {
        case FXType::Glitch:    return "Glitch";
        case FXType::ZoomPunch: return "Zoom Punch";
        case FXType::LUT:       return "LUT Grade";
        case FXType::LightLeak: return "Light Leak";
        case FXType::VHS:       return "VHS";
        case FXType::Datamosh:  return "Datamosh";
        case FXType::ChromaKey: return "Chroma Key";
#include "generated/fx_ui_label.h"
        default:                return "FX";
    }
}

ImU32 clip_badge_color(const Clip& c) {
    if (c.clip_type == ClipType::Effect) return fx_type_accent(c.fx_type);
    return ImGui::ColorConvertFloat4ToU32(clip_type_badge_color(c.clip_type));
}

const char* clip_display_name(const Clip& c) {
    if (c.clip_type == ClipType::Effect) return fx_type_name(c.fx_type);
    return clip_type_name(c.clip_type);
}

bool fx_type_is_adjustment_style(FXType ft) {
    switch (ft) {
        case FXType::Duotone:          case FXType::Posterize:      case FXType::BleachBypass:
        case FXType::ColorBurn:        case FXType::Solarize:       case FXType::Daguerreotype:
        case FXType::XRay:             case FXType::MiamiVice:      case FXType::HorrorGrade:
        case FXType::SplitToning:      case FXType::DesertGold:     case FXType::GradientMap:
        case FXType::CrossProcess:     case FXType::Technicolor:    case FXType::Kodachrome:
        case FXType::SepiaRich:        case FXType::ColorDodge:     case FXType::InfraredFilm:
        case FXType::Thermal:          case FXType::ThermalMap:     case FXType::VintageNegative:
        case FXType::GoldenHour:       case FXType::CyberpunkGrade: case FXType::ZoneSystemBw:
        case FXType::WarholPop:        case FXType::DitherBayer:    case FXType::BitCrush:
            return true;
        default: return false;
    }
}

FxBrickColors fx_brick_colors(FXType ft, bool sel) {
    int r, g, b;
    if (ft == FXType::Grade || ft == FXType::Blur || ft == FXType::Vignette ||
        fx_type_is_adjustment_style(ft)) { r=100; g=80;  b=200; }
    else                                 { r=210; g=110; b=30;  }
    if (sel) {
        int rb = (int)fminf(r*1.25f,255), gb2 = (int)fminf(g*1.25f,255), bb = (int)fminf(b*1.25f,255);
        return { IM_COL32(r,g,b,210), IM_COL32(rb,gb2,bb,255), IM_COL32(240,240,255,240) };
    }
    return { IM_COL32(r/4,g/4,b/4,130), IM_COL32(r*2/3,g*2/3,b*2/3,200), IM_COL32(r,g,b,220) };
}
