// screen_studio.cpp — coordinator: layout, shortcuts, menu bar, ui_studio entry point
#include "screens.h"
#include "studio_types.h"
#include "studio_shared.h"
#include "canvas.h"
#include "timeline.h"
#include "pipeline.h"
#include "panel_clip.h"
#include "panel_animation.h"
#include "panel_fx.h"
#include "panel_media.h"
#include "panel_terminal.h"
#include "panel_agent.h"
#include "../agent_harness.h"
#include "../recorder.h"
#include "../video_recorder.h"
#include "export_ui.h"
#include "theme.h"
#include "app.h"
#include "audio.h"
#include "video.h"
#include "proxy.h"
#include "bg_remove.h"
#include "vc_job.h"
#include "noise_reduce.h"
#include "transcribe.h"
#include "filepicker.h"
#include "paths.h"

// Default location offered by the Save dialog: the existing project path, or a
// fresh name under ~/Videos/Pop Maker Studio Projects.
static std::string project_save_default(const AppState& s) {
    return s.project_path.empty() ? (projects_dir() + "/Untitled.pms") : s.project_path;
}
#include "globals.h"
#include "render.h"
#include "history.h"
#include "blender_export.h"
#include "project.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <set>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <chrono>

namespace fs = std::filesystem;

extern ImFont* g_font_bold;

extern ImFont* g_font_black;

// ── Panel view — single source of truth for the right-hand panel ─────────────

// s_panel_view — declared extern in studio_shared.h for helpers that switch it
PanelView s_panel_view = PanelView::Project;

// One-shot panel-view request (see request_panel_view in studio_shared.h). The
// panel router checks this before deriving a view from the selection so a lane
// click can land directly on the FX tab. -1 sentinel = no request pending.
static int s_panel_request = -1;
void request_panel_view(PanelView v) { s_panel_request = (int)v; }
// "Hear effects" sync: keep the live monitor chain matched to the record
// brick's audio FX (the recording target, else the selected Record brick,
// else the first one). Rebuilds only when the effective chain changes.
static void monitor_chain_sync(AppState& state) {
    static uint64_t s_last_chain_hash = 0;
    if (!audio_capture_active() || !audio_monitor_fx_get()) {
        if (s_last_chain_hash) { audio_monitor_chain_set({}); s_last_chain_hash = 0; }
        return;
    }
    // Both record bricks monitor through this chain — Audio Record and Video
    // Record (its mic) share the same "Hear effects" path and the same coupled
    // audio-FX. Treat either as a candidate, else a Video Rec brick's effects
    // are never heard while monitoring.
    auto is_rec = [](ClipType t) {
        return t == ClipType::Record || t == ClipType::VideoRecord;
    };
    int bti = -1, bci = -1;
    for (int ti = 0; ti < (int)state.tracks.size() && bti < 0; ++ti)
        for (int ci = 0; ci < (int)state.tracks[ti].clips.size(); ++ci)
            if (is_rec(state.tracks[ti].clips[ci].clip_type) &&
                (recorder_is_target(ti, ci) || vrecorder_is_target(ti, ci)))
                { bti = ti; bci = ci; break; }
    if (bti < 0 && state.selected_track >= 0 &&
        state.selected_track < (int)state.tracks.size() &&
        state.selected_clip >= 0 &&
        state.selected_clip < (int)state.tracks[state.selected_track].clips.size() &&
        is_rec(state.tracks[state.selected_track].clips[state.selected_clip].clip_type)) {
        bti = state.selected_track; bci = state.selected_clip;
    }
    if (bti < 0)
        for (int ti = 0; ti < (int)state.tracks.size() && bti < 0; ++ti)
            for (int ci = 0; ci < (int)state.tracks[ti].clips.size(); ++ci)
                if (is_rec(state.tracks[ti].clips[ci].clip_type))
                    { bti = ti; bci = ci; break; }

    // The monitor is fed from the SAME windows the take/export use, gated on the
    // live playhead: each coupled FX activates over its own span with the same
    // edge crossfades as playback — whether you're recording, playing, or just
    // scrubbing. So "what you monitor is what the take becomes", and to dial in
    // / preview an effect you park the playhead inside its brick (a brick that
    // spans the whole take is "always on"). The segments don't change as the
    // playhead moves, so this rebuilds only when a brick is edited; the audio
    // thread does the per-sample span gating.
    std::vector<AudioFXSegment> win_segs;
    float brick_start = 0.f;
    uint64_t h = 1469598103934665603ull;
    if (bti >= 0) {
        const Clip& rbrick = state.tracks[bti].clips[bci];
        brick_start = rbrick.start;
        auto segs = collect_audio_fx_segments(state, bti, rbrick);
        for (auto& sg : segs) {
            if (!sg.fx.any_active()) continue;
            win_segs.push_back(sg);
            h = (h ^ audio_fx_hash(sg.fx)) * 1099511628211ull;
            // Window bounds feed the hash so moving/resizing a brick rebuilds.
            h = (h ^ (uint64_t)(sg.t0 * 1000.f)
                   ^ ((uint64_t)(sg.t1 * 1000.f) << 21)) * 1099511628211ull;
        }
    }
    if (h != s_last_chain_hash) {
        audio_monitor_chain_set_seg(win_segs, brick_start);
        s_last_chain_hash = h;
    }
}

static bool s_user_nav = false; // user explicitly chose Animation/History tab
// Coupled Multi-FX brick shown in the selected content's FX tab this frame.
static int  s_host_fx_ti  = -1;
static int  s_host_fx_ci  = -1;
static int  s_host_afx_ci = -1;

static void handle_shortcuts(AppState& state) {
    if (terminal_is_focused()) return;
    ImGuiIO& io = ImGui::GetIO();

    // Arrow-key playhead nudge — runs before the IsAnyItemActive guard so a
    // stale slider activation can't lock the user out of seeking. Shift = 5s.
    // Plain step scales to timeline zoom so the playhead moves at least ~5px
    // per press, but never less than one video frame.
    if (!io.WantTextInput) {
        float fps_a  = tl_fps(state);
        float f_dt_a = fps_a > 0.f ? 1.f / fps_a : 1.f / 30.f;
        float dur_a  = fmaxf(state.duration, 0.f);
        bool right = ImGui::IsKeyPressed(ImGuiKey_RightArrow, true);
        bool left  = ImGui::IsKeyPressed(ImGuiKey_LeftArrow,  true);
        if (right || left) {
            float step;
            if (io.KeyShift) {
                step = 5.f;
            } else {
                float zoom_px_per_sec = state.tl_zoom;
                float visible_step = (zoom_px_per_sec > 0.001f) ? 5.f / zoom_px_per_sec : f_dt_a;
                step = fmaxf(f_dt_a, visible_step);
            }
            float ph = state.playhead + (right ? step : -step);
            seek_to(state, fmaxf(0.f, fminf(ph, dur_a)));
            if (ImGui::GetActiveID() != 0) ImGui::ClearActiveID();
            return;
        }
    }

    if (ImGui::IsAnyItemActive()) return;

    // ── Undo / Redo ───────────────────────────────────────────────────────────
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) {
        history_undo(state); return;
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z) ||
        ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y)) {
        history_redo(state); return;
    }

    // ── Save / Open ───────────────────────────────────────────────────────────
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) {
        if (state.project_path.empty())
            state.project_path = filepicker_save("Save project", "PMS Project", "*.pms", project_save_default(state).c_str());
        if (!state.project_path.empty()) { project_save(state, state.project_path); recent_projects_push(state.project_path); recovery_clear(); mark_project_saved(state, state.project_path); }
        return;
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S)) {
        std::string p = filepicker_save("Save project as", "PMS Project", "*.pms", project_save_default(state).c_str());
        if (!p.empty()) { state.project_path = p; project_save(state, p); recent_projects_push(p); recovery_clear(); mark_project_saved(state, p); }
        return;
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_N)) {
        enter_new_project(state);  // preserves models flags + keeps us in the editor
        return;
    }

    // ── Playback ──────────────────────────────────────────────────────────────
    float fps    = tl_fps(state);
    float f_dt   = fps > 0.f ? 1.f / fps : 1.f / 30.f;
    float dur    = fmaxf(state.duration, 0.f);

    if (ImGui::IsKeyPressed(ImGuiKey_Space) ||
        ImGui::IsKeyPressed(ImGuiKey_K)) {
        toggle_play(state); return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_L) && !state.playing) {
        toggle_play(state); return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Home)) { seek_to(state, 0.f);  return; }
    if (ImGui::IsKeyPressed(ImGuiKey_End))  { seek_to(state, dur);  return; }

    // Markers / locators: M drops one at the playhead, [ / ] jump between them.
    if (ImGui::IsKeyPressed(ImGuiKey_M))            { marker_add(state, state.playhead); return; }
    if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket))  { marker_jump(state, -1); return; }
    if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) { marker_jump(state, +1); return; }

    // ── Clip operations (need a selected clip) ────────────────────────────────
    if (state.selected_track<0 || state.selected_clip<0) return;
    if (state.selected_track>=(int)state.tracks.size()) return;
    Track& track = state.tracks[state.selected_track];
    if (state.selected_clip>=(int)track.clips.size()) return;
    Clip& clip = track.clips[state.selected_clip];

    if (track.locked) return;

    if (ImGui::IsKeyPressed(ImGuiKey_S) ||
        ImGui::IsKeyChordPressed(ImGuiMod_Ctrl|ImGuiKey_B)) {
        float cut = state.playhead;
        if (cut > clip.start + f_dt && cut < clip.end - f_dt) {
            clip_split_with_fx(state, state.selected_track, state.selected_clip, cut);
            history_push(state, "Split clip");
        }
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Delete) ||
        ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
        bool multi = state.clip_selection.size() > 1;
        if (delete_selected_clips(state))
            history_push(state, multi ? "Delete clips" : "Delete clip");
        return;
    }
}
// ── API provider presets + brand glyphs (Settings > Agent) ────────────────────
// Each provider is an OpenAI-compatible endpoint. Glyphs are simplified brand
// marks drawn with ImDrawList — no asset files, crisp at any size.
namespace {
constexpr float PI_F = 3.14159265358979f;

void glyph_anthropic(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
    // 6-arm burst (three crossing strokes) — Anthropic's asterisk-like mark.
    const float th = r * 0.30f;
    const float deg[3] = { 90.f, 30.f, 150.f };
    for (float dgr : deg) {
        float a = dgr * PI_F / 180.f;
        ImVec2 d = { cosf(a), -sinf(a) };
        dl->AddLine({c.x - d.x*r, c.y - d.y*r}, {c.x + d.x*r, c.y + d.y*r}, col, th);
        dl->AddCircleFilled({c.x + d.x*r, c.y + d.y*r}, th*0.5f, col);
        dl->AddCircleFilled({c.x - d.x*r, c.y - d.y*r}, th*0.5f, col);
    }
}
void glyph_openai(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
    // Hexagonal knot → hexagon ring with vertex nodes and an inner ring.
    const float th = r * 0.16f;
    dl->AddNgon(c, r, col, 6, th);
    for (int i = 0; i < 6; ++i) {
        float a = (i / 6.f) * 2 * PI_F - PI_F/2;
        dl->AddCircleFilled({c.x + cosf(a)*r, c.y + sinf(a)*r}, th*0.9f, col);
    }
    dl->AddNgon(c, r*0.42f, col, 6, th*0.85f);
}
void glyph_deepseek(ImDrawList* dl, ImVec2 c, float r, ImU32 col, ImU32 bg) {
    // Minimal blue whale: round body + tail fluke + spout + eye.
    ImVec2 bc = { c.x - r*0.12f, c.y + r*0.08f };
    float  br = r * 0.70f;
    dl->AddCircleFilled(bc, br, col);
    dl->AddTriangleFilled({c.x + r*0.40f, c.y + r*0.02f},
                          {c.x + r*0.98f, c.y - r*0.52f},
                          {c.x + r*0.98f, c.y + r*0.34f}, col);
    dl->AddLine({bc.x - br*0.15f, bc.y - br*0.95f},
                {bc.x - br*0.15f, bc.y - br*1.35f}, col, r*0.12f);
    dl->AddCircleFilled({bc.x - br*0.28f, bc.y - br*0.12f}, br*0.16f, bg);
}
void glyph_gemini(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
    // Four-point spark (concave star) drawn as a center-fan of triangles.
    ImVec2 p[8];
    float inner = r * 0.34f;
    for (int i = 0; i < 8; ++i) {
        float a = (i / 8.f) * 2 * PI_F - PI_F/2;
        float rad = (i % 2 == 0) ? r : inner;
        p[i] = { c.x + cosf(a)*rad, c.y + sinf(a)*rad };
    }
    for (int i = 0; i < 8; ++i)
        dl->AddTriangleFilled(c, p[i], p[(i+1)%8], col);
}
void glyph_custom(ImDrawList* dl, ImVec2 c, float r, ImU32 col, ImU32 bg) {
    // Three sliders — a clean "settings / custom endpoint" mark.
    const float th = r * 0.13f;
    for (int i = 0; i < 3; ++i) {
        float y  = c.y - r*0.55f + i * (r*0.55f);
        dl->AddLine({c.x - r*0.85f, y}, {c.x + r*0.85f, y}, col, th);
        float kx = c.x + (i % 2 == 0 ? -r*0.38f : r*0.38f);
        dl->AddCircleFilled({kx, y}, th*1.7f, col);
        dl->AddCircleFilled({kx, y}, th*0.95f, bg);
    }
}

struct ApiProvider {
    const char* name;       // short label on the card
    const char* full;       // full company name (tooltip / dropdown)
    const char* base_url;   // OpenAI-compatible endpoint
    const char* models[4];  // suggested model ids (nullptr-terminated)
    ImU32       color;      // brand accent
    int         kind;       // 0 anthropic,1 openai,2 deepseek,3 gemini,4 custom
};
const ApiProvider kProviders[] = {
    { "Claude",   "Anthropic", "https://api.anthropic.com/v1/",
      { "claude-sonnet-4-5", "claude-opus-4-1", "claude-3-5-haiku-latest", nullptr },
      IM_COL32(217, 119, 87, 255), 0 },
    { "OpenAI",   "OpenAI", "https://api.openai.com/v1",
      { "gpt-4o", "gpt-4o-mini", "o3-mini", nullptr },
      IM_COL32(16, 163, 127, 255), 1 },
    { "DeepSeek", "DeepSeek", "https://api.deepseek.com",
      { "deepseek-chat", "deepseek-reasoner", nullptr, nullptr },
      IM_COL32(77, 107, 254, 255), 2 },
    { "Gemini",   "Google", "https://generativelanguage.googleapis.com/v1beta/openai/",
      { "gemini-2.0-flash", "gemini-1.5-pro", "gemini-1.5-flash", nullptr },
      IM_COL32(66, 133, 244, 255), 3 },
    { "Custom",   "Custom endpoint", "",
      { nullptr, nullptr, nullptr, nullptr },
      IM_COL32(150, 150, 165, 255), 4 },
};
constexpr int kProviderCount = (int)(sizeof(kProviders)/sizeof(kProviders[0]));

void draw_provider_glyph(ImDrawList* dl, int kind, ImVec2 c, float r, ImU32 col, ImU32 bg) {
    switch (kind) {
        case 0: glyph_anthropic(dl, c, r, col); break;
        case 1: glyph_openai(dl, c, r, col); break;
        case 2: glyph_deepseek(dl, c, r, col, bg); break;
        case 3: glyph_gemini(dl, c, r, col); break;
        default: glyph_custom(dl, c, r, col, bg); break;
    }
}
// Which preset matches the current base URL? -1 → Custom (last entry).
int provider_for_url(const std::string& url) {
    for (int i = 0; i < kProviderCount; ++i)
        if (kProviders[i].base_url[0] && url == kProviders[i].base_url) return i;
    return kProviderCount - 1;  // Custom
}
} // namespace

// Natural ("01, 02, 10" not "01, 10, 2") filename order for multi-file drops, so
// numbered b-roll sequences in the right order. Case-insensitive; compares digit
// runs by numeric value with leading zeros ignored.
static bool natural_less(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        bool da = isdigit((unsigned char)a[i]), db = isdigit((unsigned char)b[j]);
        if (da && db) {
            size_t i2 = i, j2 = j;
            while (i2 < a.size() && isdigit((unsigned char)a[i2])) ++i2;
            while (j2 < b.size() && isdigit((unsigned char)b[j2])) ++j2;
            size_t ia = i; while (ia + 1 < i2 && a[ia] == '0') ++ia;
            size_t ja = j; while (ja + 1 < j2 && b[ja] == '0') ++ja;
            size_t la = i2 - ia, lb = j2 - ja;
            if (la != lb) return la < lb;                       // longer number = larger
            int c = a.compare(ia, la, b, ja, lb);
            if (c != 0) return c < 0;
            i = i2; j = j2;
        } else {
            char ca = (char)tolower((unsigned char)a[i]);
            char cb = (char)tolower((unsigned char)b[j]);
            if (ca != cb) return ca < cb;
            ++i; ++j;
        }
    }
    return (a.size() - i) < (b.size() - j);
}

void ui_studio(AppState& state) {
    ImGuiIO& io   = ImGui::GetIO();
    float    win_w = io.DisplaySize.x;
    float    win_h = io.DisplaySize.y;

    // Stop scrub audio once the blip window expires.
    if (s_scrub_until > 0.0 && ImGui::GetTime() >= s_scrub_until && !state.playing) {
        audio_pause();
        s_scrub_until = 0.0;
    }

    handle_shortcuts(state);
    // (poll_clip_beat_analysis runs in engine_tick)

    // Keep state.duration in sync with actual clip content every frame.
    if (!state.tracks.empty()) {
        float pe = project_end(state);
        if (pe > 0.01f) state.duration = pe;
    }

    // Handle OS drop — SRT, audio, or video.
    // If the terminal is focused, all drops belong to it so the studio
    // handler stands down. The terminal panel will inject the path at the
    // shell prompt later this frame.
    extern std::string g_dropped_file;
    extern std::vector<std::string> g_drop_batch;
    bool term_claims_drop  = state.terminal_open && terminal_is_focused();
    bool agent_claims_drop = state.agent_panel_open && agent_input_is_focused();
    bool editor_owns_drop  = !term_claims_drop && !agent_claims_drop;
    // A "batch" drop is multiple files, or a single folder. Those go to the Bin
    // (and sequence over a track) rather than single-clip placement below.
    bool is_batch_drop = g_drop_batch.size() > 1 ||
                         (g_drop_batch.size() == 1 && path_is_dir(g_drop_batch[0]));
    if (!g_dropped_file.empty() && editor_owns_drop && !is_batch_drop) {
        const std::string& dp = g_dropped_file;
        fs::path fp(dp);
        std::string ext = fp.extension().string();
        for (auto& c : ext) c = (char)tolower((unsigned char)c);

        if (ext == ".srt") {
            // Parse SRT and place clips on hovered track or a new track.
            auto clips = parse_srt(dp);
            if (!clips.empty()) {
                int target = -1;
                if (s_tl_hover_track >= 0 && s_tl_hover_track < (int)state.tracks.size()) {
                    target = s_tl_hover_track;
                    Track& tr = state.tracks[target];
                    tr.clips.insert(tr.clips.end(), clips.begin(), clips.end());
                } else {
                    Track t;
                    t.name = fp.stem().string();
                    t.clips = clips;
                    state.tracks.insert(state.tracks.begin(), std::move(t));
                    target = 0;
                }
                s_drop_flash_track = target;
                s_drop_flash_t     = 0.6f;
                history_push(state, "Import SRT \"" + fp.filename().string() + "\"");
            }
        } else if (is_image_path(dp)) {
            // Dropped image: 5-second clip at playhead on an empty track
            // (new track at top only when none is free), same as Browse
            recent_media_push(dp, MediaKind::Image);
            bin_add(state, dp);
            Clip cl;
            cl.clip_type = ClipType::Video;
            cl.text      = dp;
            cl.source_id = dp;
            cl.start     = state.playhead;
            cl.end       = cl.start + 5.f;
            int target = find_empty_track(state);
            if (target < 0) {
                Track nt; nt.name = fp.stem().string();
                state.tracks.insert(state.tracks.begin(), std::move(nt));
                target = 0;
            }
            state.tracks[target].clips.push_back(cl);
            state.selected_track = target;
            state.selected_clip  = (int)state.tracks[target].clips.size() - 1;
            proxy_start(dp);
            int slot = slot_for_video(state, clip_slot_key(dp, cl.start), dp);
            if (slot >= 0) video_open_still(slot, proxy_still_path(dp));
            state.video_loaded = true;
            s_drop_flash_track = target;
            s_drop_flash_t     = 0.6f;
            history_push(state, "Import image: " + fp.filename().string());
        } else if (is_audio_file(dp)) {
            bool is_vid = (ext==".mp4"||ext==".mov"||ext==".mkv"||ext==".avi"||ext==".webm");
            ClipType drop_ct = is_vid ? ClipType::Video : ClipType::Audio;
            bin_add(state, dp);

            if (s_tl_hover_track >= 0 && s_tl_hover_track < (int)state.tracks.size()) {
                add_clip_to_track(state, s_tl_hover_track, dp, drop_ct, /*reveal=*/false);
                if (state.audio_path.empty()) {
                    state.audio_path = dp;
                    audio_load(dp);
                    if (state.duration <= 0.f) state.duration = audio_duration();
                }
                s_drop_flash_track = s_tl_hover_track;
            } else {
                import_file(state, dp);
                s_drop_flash_track = (int)state.tracks.size() - 1;
            }
            s_drop_flash_t = 0.6f;
        }
        g_dropped_file.clear();
    }

    // ── Multi-file / folder drop on the editor ────────────────────────────────
    // Every dropped media file lands in the Bin (folders expand to their media).
    // When the cursor is over a timeline track, also lay the visual media
    // end-to-end on that track from the playhead — a quick sequence — in natural
    // filename order. Off-track, it's Bin-only (the "gather assets" path).
    if (is_batch_drop && editor_owns_drop) {
        std::vector<std::string> media;
        bool had_dir = false;
        for (auto& p : g_drop_batch) {
            if (path_is_dir(p)) {
                had_dir = true;
                for (auto& f : dir_media_files(p)) media.push_back(f);
            } else if (is_media_path(p)) {
                media.push_back(p);
            }
        }
        std::sort(media.begin(), media.end(), natural_less);
        for (auto& m : media) bin_add(state, m);   // ensure binned (covers single-folder)

        bool over_track = s_tl_hover_track >= 0 &&
                          s_tl_hover_track < (int)state.tracks.size();
        if (over_track) {
            int target = s_tl_hover_track;
            float cursor = state.playhead;
            int placed = 0;
            for (auto& m : media) {
                if (kind_for_path(m) == MediaKind::Audio) continue;  // audio stays in the Bin
                float dur = video_probe_duration(m);                 // images → 0 → default
                if (dur <= 0.f) dur = is_image_path(m) ? 5.f : 4.f;
                Clip cl;
                cl.clip_type = ClipType::Video;
                cl.text = m; cl.source_id = m;
                cl.start = cursor; cl.end = cursor + dur;
                state.tracks[target].clips.push_back(cl);
                proxy_start(m);
                int slot = slot_for_video(state, clip_slot_key(m, cl.start), m);
                if (slot >= 0) video_open_still(slot, proxy_still_path(m));
                cursor += dur; ++placed;
            }
            if (placed > 0) {
                state.video_loaded = true;
                s_drop_flash_track = target;
                s_drop_flash_t     = 0.6f;
                history_push(state, "Sequence " + std::to_string(placed) +
                                    " clips on " + state.tracks[target].name);
            }
        }
        // A dropped directory dumps its media into the Bin — surface it by
        // switching the panel to the Bin tab so the user sees what landed.
        if (had_dir) s_panel_view = PanelView::LibBin;

        g_dropped_file.clear(); // a single-folder drop set this; we handled it
        g_drop_batch.clear();   // consumed; don't let it linger to the agent path
    }

    // Per-slot video open/upgrade — handles four states:
    //   1. Already on the Proxy tier        → nothing to do (steady state)
    //   2. Proxy ready, slot is Still/Native → upgrade to Proxy (fastest scrub)
    //   3. No proxy yet, slot unopened       → open Native (libav direct decode)
    //                                          for instant preview, fall back to
    //                                          Still placeholder if libav can't
    //                                          open the file
    // Images never get an MJPEG proxy — skip them to avoid per-frame ffprobe spawns.
    for (int slot = 0; slot < MAX_VIDEO_SLOTS; ++slot) {
        const std::string& key = state.proxy_paths[slot];
        if (key.empty()) continue;
        if (video_source(slot) == PreviewSource::Proxy) continue;  // terminal state

        std::string src = source_from_key(key);
        if (is_animated_image(src)) {
            // GIF: decode to full-res RGBA frames once (lossless + alpha) and show
            // the frame at the playhead — no lossy mp4 conform / MJPEG proxy that
            // softened them and dropped transparency.
            if (!video_is_gif(slot)) video_open_gif(slot, src);
            continue;
        }
        if (is_image_path(src) && !is_animated_image(src)) {
            // Still images never get an MJPEG proxy — keep them out of the
            // generic native/proxy logic below (per-frame libav opens).
            // Animated images (.gif) fall through to the proxy path so they
            // play instead of freezing on frame 0.
            // Still is their terminal state; repair both Closed slots (the add-time open
            // races the background still generator for brand-new files) and
            // slots stuck in Native (libav opens a PNG as a one-frame video,
            // then every decode past t=0 fails and the clip renders blank).
            if (video_source(slot) != PreviewSource::Still) {
                // A still image is already an image — open the ORIGINAL at full
                // quality (PNG keeps its alpha), no lossy/downscaled JPEG still.
                video_open_still(slot, src);
                if (video_source(slot) != PreviewSource::Still) {
                    // Only formats stb_image can't read (HEIC/WEBP/TIFF) get here
                    // — fall back to the converted still proxy.
                    std::string still = proxy_still_path(src);
                    if (fs::exists(still)) {
                        video_open_still(slot, still);
                    } else {
                        // Still missing entirely (generation failed once, or the
                        // file was deleted). proxy_start regenerates it for images
                        // in a background thread, but has no in-flight dedup —
                        // kick it once per source, not per frame.
                        static std::set<std::string> s_still_kicked;
                        if (s_still_kicked.insert(src).second) proxy_start(src);
                    }
                }
            }
            continue;
        }
        if (proxy_is_ready(src)) {
            ProxyInfo pi;
            if (!proxy_load(src, pi)) continue;
            video_open_proxy(slot, pi);
            // Retry bg_remove for any clips that were waiting on this proxy
            for (int ti2 = 0; ti2 < (int)state.tracks.size(); ++ti2)
                for (int ci2 = 0; ci2 < (int)state.tracks[ti2].clips.size(); ++ci2) {
                    Clip& vc = state.tracks[ti2].clips[ci2];
                    if (vc.clip_type == ClipType::Video &&
                        vc.text == src &&
                        vc.bg_remove_status == BgRemoveStatus::WaitingForProxy)
                        bg_remove_start(state, ti2, ci2);
                }
            if (slot == 0) {
                state.proxy_ready = true;
                float pd = (float)video_info(0).duration;
                if (pd > 0.f) {
                    for (auto& tr : state.tracks)
                        for (auto& cl : tr.clips)
                            if (cl.clip_type == ClipType::Video && cl.text == src && cl.end < pd
                                && fabsf(cl.end - cl.start - 5.f) < 0.1f)
                                cl.end = pd;
                }
            }
        } else if (!video_is_open(slot)) {
            // Proxy not ready yet — open native (instant) and fall back to
            // still if libav can't read the file.
            if (!video_open_native(slot, src))
                video_open_still(slot, proxy_still_path(src));
        }
    }

    // GC slots whose clips have been deleted or moved.
    gc_video_slots(state);

    // Proxy queue: if nothing is generating, start the next pending clip.
    // Iterating every frame is cheap — proxy_is_ready is just two fs::exists calls.
    if (!proxy_is_generating()) {
        for (auto& tr : state.tracks) {
            bool started = false;
            for (auto& cl : tr.clips) {
                if (!clip_is_videolike_type(cl.clip_type)) continue;
                // Still images get no proxy; animated images (.gif) do.
                if (cl.text.empty() ||
                    (is_image_path(cl.text) && !is_animated_image(cl.text))) continue;
                if (!proxy_is_ready(cl.text)) {
                    proxy_start(cl.text);
                    started = true;
                    break;
                }
            }
            if (started) break;
        }
    }

    // IPC may have added new video clips — refresh slots through the queue
    // (drains a few per frame with the progress banner when large). The old
    // synchronous reopen_video_slots here froze the UI for seconds when the
    // flag covered a whole project's worth of sources.
    if (state.proxy_scan_needed) {
        state.proxy_scan_needed = false;
        queue_video_slot_opens(state);
    }

    // Project open in progress: open a few decoder slots per frame (each can
    // spawn an ffprobe) and surface a progress bar instead of freezing the UI.
    if (!state.slot_open_queue.empty()) {
        // (the slot-open tick itself runs in engine_tick; this is the banner)
        int done  = state.slot_open_total - (int)state.slot_open_queue.size();
        char msg[96];
        snprintf(msg, sizeof(msg), "Opening project…  %d / %d media sources",
                 done, state.slot_open_total);
        float ow = fminf(420.f, ImGui::GetIO().DisplaySize.x * 0.6f);
        ImVec2 op{(ImGui::GetIO().DisplaySize.x - ow) * 0.5f,
                  ImGui::GetIO().DisplaySize.y * 0.42f};
        float prog = state.slot_open_total > 0
                       ? (float)done / (float)state.slot_open_total : -1.f;
        ui_canvas_progress_banner(ImGui::GetForegroundDrawList(), op, ow, 0.f,
                                  msg, prog, IM_COL32(120, 170, 255, 255));
    }

    // Poll background removal and voice conversion jobs.
    bg_remove_poll(state);
    vc_poll(state);
    // Revert clips whose voice-convert FX was removed (brick deleted, chain
    // entry pulled, decoupled). Runs after vc_poll so a job that lands this
    // frame is reverted the same frame if its brick is already gone — no flash.
    vc_reconcile(state);

    // Poll noise reduction — on completion, set denoised WAV as playback source.
    {
        bool was_running = state.noise_reduce_running;
        noise_reduce_poll(state);
        if (was_running && !state.noise_reduce_running && !state.noise_reduce_output.empty()) {
            state.audio_path = state.noise_reduce_output;
            audio_load(state.audio_path);
        }
    }

    // Audio just finished loading while playing — start from current playhead
    {
        static bool s_was_loading = false;
        bool loading = audio_loading();
        if (s_was_loading && !loading && state.playing) {
            state.play_start_pos  = state.playhead;
            state.play_start_wall = std::chrono::steady_clock::now();
            audio_seek(state.playhead);
            audio_play();
        }
        s_was_loading = loading;
    }

    // Extract audio done → add Audio track directly below the source video track
    if (state.extract_done) {
        state.extract_done = false;
        if (!state.extract_wav_path.empty() && fs::exists(state.extract_wav_path)) {
            Track at;
            at.name = "Audio";
            AudioMeta meta;
            float dur = audio_probe(state.extract_wav_path, meta) ? meta.duration_secs : state.duration;
            Clip ac; ac.clip_type = ClipType::Audio;
            ac.start = 0.f; ac.end = dur; ac.text = state.extract_wav_path;
            at.clips.push_back(ac);
            // Insert directly below the source video track; fall back to bottom
            int insert_pos = (int)state.tracks.size();
            if (state.extract_source_track >= 0 &&
                state.extract_source_track < (int)state.tracks.size())
                insert_pos = state.extract_source_track + 1;
            audio_source_ensure(state.extract_wav_path);
            state.tracks.insert(state.tracks.begin() + insert_pos, std::move(at));
            state.extract_source_track = -1;
            history_push(state, "Extract audio from video");
        }
        state.extract_wav_path.clear();
    }

    // Pipeline done → apply grouping + save all SRTs + push history
    static PipelineStage last_stage = PipelineStage::Idle;
    if (last_stage != PipelineStage::Done &&
        state.pipeline.stage == PipelineStage::Done) {

        // Pipeline completion is data-only: load the transcript / save SRTs / add
        // the vocals track for SeparateOnly runs. It NEVER mutates the timeline
        // with lyric clips on its own — callers (UI buttons, MCP) decide what
        // to do next via state.pipeline_on_done.
        PipelineMode m = state.last_pipeline_mode;
        if (m == PipelineMode::TranscribeOnly) {
            state.lyrics_edits.clear();
            load_words_cache(state);
            // SRTs are no longer written here — subtitles render in-app on managed
            // tracks; an .srt is produced only on explicit export (export_ui).
        } else if (m == PipelineMode::Both) {
            state.lyrics_edits.clear();
            load_words_cache(state);
        } else if (m == PipelineMode::SeparateOnly &&
                   !state.vocals_path.empty() && fs::exists(state.vocals_path)) {
            bool already_present = false;
            for (auto& t : state.tracks)
                for (auto& c : t.clips)
                    if (c.text == state.vocals_path) { already_present = true; break; }
            if (!already_present) {
                Track vt; vt.name = "Vocals";
                AudioMeta vm;
                float vdur = audio_probe(state.vocals_path, vm) ? vm.duration_secs : state.duration;
                Clip vc; vc.clip_type = ClipType::Audio;
                vc.start = 0.f; vc.end = vdur; vc.text = state.vocals_path;
                vt.clips.push_back(vc);
                audio_source_ensure(state.vocals_path);
                state.tracks.push_back(std::move(vt));
            }
        }

        std::string stem = state.audio_path.empty() ? "audio"
            : fs::path(state.audio_path).stem().string();
        history_push(state, "Pipeline complete — " + stem);
        run_beat_detect(state);
        run_envelope_extract(state);

        // Run the caller-supplied completion action exactly once, then clear it.
        // UI buttons set this to apply_subtitle_mode or generate_typography;
        // MCP trigger_pipeline leaves it null so the timeline stays clean.
        if (state.pipeline_on_done) {
            auto cb = std::move(state.pipeline_on_done);
            state.pipeline_on_done = {};
            cb(state);
        }
    }
    last_stage = state.pipeline.stage;

    // Loop recorder: drain mic, slice takes on the loop-cycle clock.
    // (recorder_tick / vrecorder_tick run in engine_tick)
    monitor_chain_sync(state);

    // Push clip snapshots to audio system every frame.
    // The callback reads these to position audio correctly — no separate volume hack needed.
    {
        std::vector<AudioClipDesc> vdescs, adescs;
        for (auto& tr : state.tracks) {
            if (tr.muted) continue;
            int tr_idx = (int)(&tr - state.tracks.data());
            for (auto& cl : tr.clips) {
                // Record brick: the selected take plays like an audio clip.
                // Takes record at speed 1, but in_point is honored so the left
                // handle trims the take (reveals/hides content) like every other
                // brick instead of sliding the take under a fixed edge — and it
                // matches collect_audio_fx_segments, which maps FX windows
                // through in_point. Muted while that brick is recording so the
                // previous pass doesn't bleed under the new one.
                if (cl.clip_type == ClipType::Record) {
                    int ci = (int)(&cl - tr.clips.data());
                    if (cl.muted || cl.rec_take_sel < 0 ||
                        cl.rec_take_sel >= (int)cl.rec_takes.size() ||
                        recorder_is_target(tr_idx, ci)) continue;
                    AudioClipDesc d;
                    d.track    = tr_idx;
                    d.tl_start = cl.start;    d.tl_end   = cl.end;
                    d.in_point = cl.in_point; d.speed    = 1.f;
                    d.volume   = cl.volume;   d.pan      = cl.pan;
                    d.fade_in  = cl.fade_in;  d.fade_out = cl.fade_out;
                    // Keyframed volume/pan animate in the live mix too (same as
                    // audio/video clips below — audio.cpp reads vol_keys/pan_keys).
                    if (auto it = cl.ktracks.find("volume"); it != cl.ktracks.end())
                        d.vol_keys = it->second;
                    if (auto it = cl.ktracks.find("pan"); it != cl.ktracks.end())
                        d.pan_keys = it->second;
                    d.path     = cl.rec_takes[cl.rec_take_sel];
                    // Converted voice substitutes the take, same as Audio clips
                    if (cl.vc_status == VcStatus::Ready && !cl.vc_out_path.empty())
                        d.path = cl.vc_out_path;
                    // Takes get audio FX bricks on the same track (autotune
                    // over takes), windowed to each brick's range.
                    {
                        std::vector<AudioFXSegment> segs;
                        if (cl.audio_fx.any_active()) {
                            AudioFX own = cl.audio_fx;
                            own.voice_convert_on = false;
                            // Window in source time (take speed 1), offset by
                            // in_point so the brick's own chain tracks a left
                            // trim — mirrors export_fx_segments().
                            if (own.any_active())
                                segs.push_back({cl.in_point,
                                                cl.in_point + (cl.end - cl.start), own});
                        } else {
                            segs = collect_audio_fx_segments(state, tr_idx, cl);
                        }
                        if (!segs.empty()) {
                            d.fx_segs = std::move(segs);
                            d.fx_hash = audio_fx_segments_hash(d.fx_segs);
                        }
                    }
                    adescs.push_back(d);
                    audio_source_ensure(d.path);
                    continue;
                }
                if (cl.text.empty() || cl.muted) continue;
                AudioClipDesc d;
                d.track    = tr_idx;
                d.tl_start = cl.start;    d.tl_end   = cl.end;
                d.in_point = cl.in_point; d.speed    = cl.speed;
                d.volume   = cl.volume;   d.pan      = cl.pan;
                d.fade_in  = cl.fade_in;  d.fade_out = cl.fade_out;
                if (auto it = cl.ktracks.find("volume"); it != cl.ktracks.end())
                    d.vol_keys = it->second;
                if (auto it = cl.ktracks.find("pan"); it != cl.ktracks.end())
                    d.pan_keys = it->second;
                // Use converted audio when voice conversion is ready
                if (cl.clip_type == ClipType::Audio
                    && cl.vc_status == VcStatus::Ready
                    && !cl.vc_out_path.empty()) {
                    d.path = cl.vc_out_path;
                } else {
                    d.path = cl.text;
                }
                {
                    // The clip's own AudioFX chain covers its whole range;
                    // otherwise audio FX bricks apply windowed to each
                    // brick's overlap with the clip.
                    std::vector<AudioFXSegment> segs;
                    if (cl.audio_fx.any_active()) {
                        AudioFX own = cl.audio_fx;
                        if (cl.vc_status == VcStatus::Ready) own.voice_convert_on = false;
                        if (own.any_active()) {
                            float spd = fmaxf(0.01f, cl.speed);
                            segs.push_back({cl.in_point,
                                            cl.in_point + (cl.end - cl.start) * spd,
                                            own});
                        }
                    } else {
                        segs = collect_audio_fx_segments(
                            state, (int)(&tr - state.tracks.data()), cl);
                    }
                    if (!segs.empty()) {
                        d.fx_segs = std::move(segs);
                        d.fx_hash = audio_fx_segments_hash(d.fx_segs);
                    }
                }
                if (clip_is_videolike_type(cl.clip_type) && !is_image_path(cl.text)) {
                    // Video clips and camera A/V takes (.mkv with mic audio) play
                    // their audio in the live mix; photo (.jpg) takes are silent.
                    vdescs.push_back(d);
                    audio_source_ensure(cl.text);
                } else if (cl.clip_type == ClipType::Audio) {
                    adescs.push_back(d);
                    audio_source_ensure(d.path);  // loads vc_out_path when Ready, cl.text otherwise
                }
            }
        }
        video_audio_clips_update(vdescs);
        audio_clips_update(adescs);
        audio_bus_bricks_update(collect_bus_bricks(state));
    }

    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_H))
        s_panel_view = PanelView::History;

    // ── Menu bar ─────────────────────────────────────────────────────────────
    if (ImGui::BeginMenuBar()) {
        // App name
        ImGui::PushFont(g_font_black);
        ImGui::PushStyleColor(ImGuiCol_Text, Col::fg);
        ImGui::TextUnformatted("POP MAKER STUDIO");
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::SameLine(0.f, 24.f);

        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New Project", "Ctrl+N")) {
                enter_new_project(state);
            }
            if (ImGui::MenuItem("Back to Home…")) {
                state.in_studio = false;   // return to the launcher / recent-projects page
            }
            if (ImGui::MenuItem("Open Project…", "Ctrl+Shift+O")) {
                std::string picked = filepicker_open("Open project", "PMS Project", "*.pms");
                // Shared load path (teardown, async slot opens, history baseline)
                // — the same one Home cards and IPC load_project use.
                if (!picked.empty()) open_project_path(state, picked);
            }
            if (ImGui::MenuItem("Save Project", "Ctrl+S")) {
                if (state.project_path.empty())
                    state.project_path = filepicker_save("Save project", "PMS Project", "*.pms", project_save_default(state).c_str());
                if (!state.project_path.empty()) { project_save(state, state.project_path); recent_projects_push(state.project_path); recovery_clear(); mark_project_saved(state, state.project_path); }
            }
            if (ImGui::MenuItem("Save Project As…", "Ctrl+Shift+S")) {
                std::string p = filepicker_save("Save project as", "PMS Project", "*.pms", project_save_default(state).c_str());
                if (!p.empty()) { state.project_path = p; project_save(state, p); recent_projects_push(p); recovery_clear(); mark_project_saved(state, p); }
            }
            if (ImGui::MenuItem("Collect (self-contained copy)…")) {
                // Default to a fresh folder under the projects dir, named after the
                // project, with the .pms inside it — Collect drops a media/ beside it.
                std::string nm = state.project_path.empty()
                    ? std::string("Untitled")
                    : std::filesystem::path(state.project_path).stem().string();
                std::string def = projects_dir() + "/" + nm + "/" + nm + ".pms";
                std::string p = filepicker_save("Collect project into folder", "PMS Project", "*.pms", def.c_str());
                if (!p.empty()) {
                    std::string err; int copied = 0;
                    if (collect_project(state, p, err, &copied)) {
                        recent_projects_push(p);
                        recovery_clear();
                        std::string cmd = "xdg-open \"" + std::filesystem::path(p).parent_path().string() + "\" >/dev/null 2>&1 &";
                        system(cmd.c_str());
                    }
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Import Audio / Video…", "Ctrl+O")) {
                std::string picked = filepicker_open(
                    "Import audio or video",
                    "Audio & Video", "*.wav *.mp3 *.m4a *.flac *.aac *.mp4 *.mov *.mkv *.webm");
                if (!picked.empty()) import_file(state, picked);
            }
            ImGui::Separator();
            bool has_tracks = !state.tracks.empty();
            if (!has_tracks) ImGui::BeginDisabled();
            if (ImGui::MenuItem("Export SRT")) {
                std::string sp;
                if (!state.audio_path.empty())
                    sp = (fs::path(state.audio_path).parent_path() /
                         (fs::path(state.audio_path).stem().string() + ".srt")).string();
                else sp = "subtitles.srt";
                render_export_srt(state, sp);
                system(("xdg-open \"" + fs::path(sp).parent_path().string() + "\"").c_str());
            }
            if (ImGui::MenuItem("Export Blender Script")) {
                std::string sp = state.audio_path.empty() ? "pop_maker_blender.py" :
                    (fs::path(state.audio_path).parent_path() /
                     (fs::path(state.audio_path).stem().string()+"_blender.py")).string();
                blender_export_script(state, sp);
                system(("xdg-open \"" + fs::path(sp).parent_path().string() + "\"").c_str());
            }
            if (!has_tracks) ImGui::EndDisabled();
            ImGui::Separator();
            if (ImGui::MenuItem("Settings…"))
                state.show_settings_modal = true;
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Edit")) {
            bool can_undo = history_can_undo();
            bool can_redo = history_can_redo();
            if (!can_undo) ImGui::BeginDisabled();
            if (ImGui::MenuItem("Undo", "Ctrl+Z")) history_undo(state);
            if (!can_undo) ImGui::EndDisabled();
            if (!can_redo) ImGui::BeginDisabled();
            if (ImGui::MenuItem("Redo", "Ctrl+Shift+Z")) history_redo(state);
            if (!can_redo) ImGui::EndDisabled();
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Track")) {
            if (ImGui::MenuItem("Add Track")) {
                Track t;
                char n[32]; snprintf(n,sizeof(n),"Track %d",(int)state.tracks.size()+1);
                t.name=n; state.tracks.push_back(t);
                history_push(state, "Add Track");
            }
            ImGui::EndMenu();
        }

        bool has_clip = state.selected_track>=0 && state.selected_clip>=0 &&
                        state.selected_track<(int)state.tracks.size() &&
                        state.selected_clip<(int)state.tracks[state.selected_track].clips.size();
        if (ImGui::BeginMenu("Clip")) {
            if (!has_clip) { ImGui::BeginDisabled(); }
            if (ImGui::MenuItem("Split at playhead", "S") && has_clip) {
                Clip& c = state.tracks[state.selected_track].clips[state.selected_clip];
                float cut = state.playhead;
                if (cut>c.start+0.02f && cut<c.end-0.02f) {
                    clip_split_with_fx(state, state.selected_track, state.selected_clip, cut);
                    history_push(state, "Split clip");
                }
            }
            bool multi = state.clip_selection.size() > 1;
            const char* dup_label = multi ? "Duplicate clips" : "Duplicate clip";
            const char* del_label = multi ? "Delete clips"    : "Delete clip";
            if (ImGui::MenuItem(dup_label) && has_clip) {
                if (duplicate_selected_clips(state))
                    history_push(state, dup_label);
            }
            if (ImGui::MenuItem(del_label, "Del") && has_clip) {
                if (delete_selected_clips(state))
                    history_push(state, del_label);
            }
            if (!has_clip) ImGui::EndDisabled();
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Zoom in",  "Ctrl++")) state.tl_zoom = fminf(state.tl_zoom*1.25f, 4000.f);
            if (ImGui::MenuItem("Zoom out", "Ctrl+-")) state.tl_zoom = fmaxf(state.tl_zoom*0.8f,  state.tl_zoom_min);
            if (ImGui::MenuItem("Fit timeline")) {
                if (state.duration > 0.f) {
                    float avail = win_w - TL_LABEL_W - 20.f;
                    state.tl_zoom   = avail / state.duration;
                    state.tl_scroll = 0.f;
                }
            }
            ImGui::Separator();
            {
                bool soc = state.show_social_safe;
                if (ImGui::MenuItem("Social safe zones (9:16)", nullptr, soc))
                    state.show_social_safe = !soc;
                bool mm = state.show_master_meter;
                if (ImGui::MenuItem("Master meter (LUFS + peak)", nullptr, mm))
                    state.show_master_meter = !mm;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("History", "Ctrl+Shift+H")) s_panel_view = PanelView::History;
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("Getting Started…")) {
                state.show_tutorial = true;
                state.tutorial_step = 0;
            }
            ImGui::EndMenu();
        }

        // Agent + Terminal + Project + Export buttons — far right of menu bar
        {
            float btn_export_w   = 80.f;
            float btn_proj_w     = 70.f;
            float btn_term_w     = 78.f;
            float btn_agent_w    = 62.f;
            float avail = ImGui::GetContentRegionAvail().x;
            float total_btns = btn_agent_w + 6.f + btn_term_w + 6.f +
                               btn_proj_w + 6.f + btn_export_w;
            if (avail > total_btns)
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - total_btns);

            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {10.f, 2.f});

            // Accent-tint helper for toggles — snapshot BEFORE the button so
            // push/pop are always balanced.
            auto accent_btn = [&](const char* label, bool& flag) {
                bool was_open = flag;
                if (was_open) {
                    ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(60, 40, 100, 255));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(80, 55, 130, 255));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(50, 35,  85, 255));
                    ImGui::PushStyleColor(ImGuiCol_Text,          IM_COL32(200, 160, 255, 255));
                }
                if (ImGui::Button(label)) flag = !flag;
                if (was_open) ImGui::PopStyleColor(4);
            };

            bool was_agent = state.agent_panel_open;
            bool was_term  = state.terminal_open;
            accent_btn("Agent", state.agent_panel_open);
            ImGui::SameLine(0.f, 6.f);
            accent_btn("Terminal", state.terminal_open);
            // The bottom strip is single-occupancy: opening one closes the
            // other (a 50/50 split cramped both panels).
            if (state.agent_panel_open && !was_agent) {
                state.terminal_open = false;
                agent_focus_input();   // drop the caret straight into the input
            }
            if (state.terminal_open    && !was_term)  state.agent_panel_open = false;

            ImGui::SameLine(0.f, 6.f);
            if (ImGui::Button("Project")) {
                s_panel_view = PanelView::Project;
                state.selected_track = -1;
                state.selected_clip  = -1;
                state.clip_selection.clear();
            }
            ImGui::SameLine(0.f, 6.f);

            ImGui::PushStyleColor(ImGuiCol_Button,        Col::fg);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Col::bg_soft_hov);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  Col::line);
            ImGui::PushStyleColor(ImGuiCol_Text,          Col::bg);
            if (ImGui::Button("Export")) state.show_export_modal = true;
            ImGui::PopStyleColor(4);
            ImGui::PopStyleVar();
        }

        ImGui::EndMenuBar();
    }

    ui_model_download_modal(state);
    draw_export_modal(state);

    // ── Settings modal ────────────────────────────────────────────────────────
    if (state.show_settings_modal) {
        ImGui::OpenPopup("##settings_modal");
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Always, {0.5f, 0.5f});
        ImGui::SetNextWindowSize({500.f, 0.f});
        ImGui::PushStyleColor(ImGuiCol_PopupBg, to_u32(Col::bg));
        ImGui::PushStyleColor(ImGuiCol_Border,  to_u32(Col::line));

        if (ImGui::BeginPopupModal("##settings_modal", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize)) {

            ImGui::Dummy({0.f, 12.f});
            ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x + 8.f);
            ImGui::PushFont(g_font_bold);
            ImGui::TextUnformatted("Settings");
            ImGui::PopFont();

            ImGui::Dummy({0.f, 12.f});
            ui_separator();
            ImGui::Dummy({0.f, 10.f});

            // ── Agent (in-app AI) ─────────────────────────────────────────────
            ui_label("Agent");
            ImGui::Dummy({0.f, 4.f});
            {
                float lx = ImGui::GetStyle().WindowPadding.x + 8.f;
                static char s_key_buf[256] = {};
                AgentConfig acfg = agent_get_config();
                static char s_url_buf[256] = {};
                static char s_model_buf[128] = {};
                static bool s_synced = false;
                static int  s_prov_sel = -1;  // -1 → derive from base URL
                if (!s_synced) {
                    strncpy(s_url_buf,   acfg.base_url.c_str(), sizeof(s_url_buf)-1);
                    strncpy(s_model_buf, acfg.model.c_str(),    sizeof(s_model_buf)-1);
                    s_synced = true;
                }
                ImU32 bgc = to_u32(Col::bg);

                // ── Provider card grid ────────────────────────────────────────
                ImGui::SetCursorPosX(lx);
                ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
                ImGui::TextUnformatted("Provider"); ImGui::PopStyleColor();
                ImGui::Dummy({0.f, 4.f});

                int sel = (s_prov_sel >= 0) ? s_prov_sel : provider_for_url(acfg.base_url);
                ImVec2 grid0 = ImGui::GetCursorPos();
                grid0.x = lx;
                const float cw = 144.f, ch = 60.f, gp = 10.f;
                const int   pr = 3;
                ImDrawList* dl = ImGui::GetWindowDrawList();
                for (int i = 0; i < kProviderCount; ++i) {
                    const ApiProvider& pv = kProviders[i];
                    int rr = i / pr, cc = i % pr;
                    ImGui::SetCursorPos({grid0.x + cc*(cw+gp), grid0.y + rr*(ch+gp)});
                    ImGui::PushID(i);
                    ImGui::InvisibleButton("##prov", {cw, ch});
                    bool hov = ImGui::IsItemHovered();
                    bool clk = ImGui::IsItemClicked();
                    if (hov) ImGui::SetTooltip("%s", pv.full);
                    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
                    bool on = (i == sel);
                    dl->AddRectFilled(a, b, hov ? IM_COL32(40,40,52,255)
                                                : IM_COL32(28,28,37,255), 9.f);
                    if (on) dl->AddRectFilled(a, b, (pv.color & 0x00FFFFFFu) | (38u<<24), 9.f);
                    dl->AddRect(a, b, on ? pv.color : IM_COL32(62,62,80,255),
                                9.f, 0, on ? 2.f : 1.f);
                    draw_provider_glyph(dl, pv.kind, {a.x + cw*0.5f, a.y + 21.f},
                                        12.f, pv.color, bgc);
                    ImVec2 ts = ImGui::CalcTextSize(pv.name);
                    dl->AddText({a.x + (cw-ts.x)*0.5f, b.y - 19.f},
                                on ? IM_COL32(240,240,250,255) : to_u32(Col::muted), pv.name);
                    ImGui::PopID();
                    if (clk && i != sel) {
                        s_prov_sel = i;
                        if (pv.base_url[0]) {
                            acfg.base_url = pv.base_url;
                            strncpy(s_url_buf, pv.base_url, sizeof(s_url_buf)-1);
                            s_url_buf[sizeof(s_url_buf)-1] = 0;
                        }
                        if (pv.models[0]) {
                            acfg.model = pv.models[0];
                            strncpy(s_model_buf, pv.models[0], sizeof(s_model_buf)-1);
                            s_model_buf[sizeof(s_model_buf)-1] = 0;
                        }
                        agent_set_config(acfg);
                        sel = i;
                    }
                }
                int rows = (kProviderCount + pr - 1) / pr;
                ImGui::SetCursorPos({grid0.x, grid0.y + rows*(ch+gp) + 2.f});

                // ── Custom endpoint: editable Base URL ────────────────────────
                if (kProviders[sel].kind == 4) {
                    ImGui::SetCursorPosX(lx);
                    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
                    ImGui::TextUnformatted("Base URL"); ImGui::PopStyleColor();
                    ImGui::SameLine(0.f, 8.f);
                    ImGui::SetNextItemWidth(228.f);
                    if (ImGui::InputText("##agent_url", s_url_buf, sizeof(s_url_buf))) {
                        acfg.base_url = s_url_buf; agent_set_config(acfg);
                        s_prov_sel = -1;  // re-derive (a pasted known URL re-highlights)
                    }
                }

                // ── Model: editable field + suggestions dropdown ──────────────
                ImGui::SetCursorPosX(lx);
                ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
                ImGui::TextUnformatted("Model   "); ImGui::PopStyleColor();
                ImGui::SameLine(0.f, 8.f);
                ImGui::SetNextItemWidth(200.f);
                if (ImGui::InputText("##agent_model", s_model_buf, sizeof(s_model_buf))) {
                    acfg.model = s_model_buf; agent_set_config(acfg);
                }
                ImGui::SameLine(0.f, 4.f);
                if (ImGui::Button("\xe2\x96\xbc##model_sug")) ImGui::OpenPopup("##model_sug");
                if (ImGui::BeginPopup("##model_sug")) {
                    const ApiProvider& pv = kProviders[sel];
                    bool any = false;
                    for (int m = 0; m < 4 && pv.models[m]; ++m) {
                        any = true;
                        if (ImGui::MenuItem(pv.models[m])) {
                            strncpy(s_model_buf, pv.models[m], sizeof(s_model_buf)-1);
                            s_model_buf[sizeof(s_model_buf)-1] = 0;
                            acfg.model = pv.models[m]; agent_set_config(acfg);
                        }
                    }
                    if (!any) ImGui::TextDisabled("no suggestions — type a model id");
                    ImGui::EndPopup();
                }

                // ── API key (per provider, stored in the system keyring) ──────
                ImGui::Dummy({0.f, 6.f});
                ImGui::SetCursorPosX(lx);
                if (!agent_key_available()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
                    ImGui::TextWrapped("secret-tool not found — install "
                        "libsecret to store the API key in the system keyring.");
                    ImGui::PopStyleColor();
                } else {
                    bool has_key = agent_key_present();
                    ImGui::PushStyleColor(ImGuiCol_Text,
                        has_key ? IM_COL32(100, 220, 130, 255) : to_u32(Col::muted));
                    ImGui::TextUnformatted(has_key ? "API key: stored in keyring"
                                                   : "API key: not set");
                    ImGui::PopStyleColor();
                    ImGui::SetCursorPosX(lx);
                    ImGui::SetNextItemWidth(260.f);
                    ImGui::InputText("##agent_key", s_key_buf, sizeof(s_key_buf),
                                     ImGuiInputTextFlags_Password);
                    ImGui::SameLine(0.f, 6.f);
                    if (ui_btn("Save key", false, true) && s_key_buf[0]) {
                        if (agent_key_store(s_key_buf))
                            memset(s_key_buf, 0, sizeof(s_key_buf));
                    }
                    if (has_key) {
                        ImGui::SameLine(0.f, 6.f);
                        if (ui_btn("Clear", false, true)) agent_key_clear();
                    }
                }

                // ── Apply & Test connection ───────────────────────────────────
                ImGui::Dummy({0.f, 8.f});
                ImGui::SetCursorPosX(lx);
                if (ui_btn("Apply & Test", false, true)) {
                    acfg.base_url = s_url_buf;
                    acfg.model    = s_model_buf;
                    agent_set_config(acfg);    // commit + persist
                    agent_test_begin(acfg);    // confirm model + url + key actually work
                }
                {
                    std::string tmsg; int tst = agent_test_state(tmsg);
                    if (tst != 0) {
                        ImGui::SetCursorPosX(lx);
                        ImU32 c = (tst == 1) ? to_u32(Col::muted)
                                : (tst == 2) ? IM_COL32(100, 220, 130, 255)
                                             : IM_COL32(235, 110, 110, 255);
                        const char* pfx = (tst == 2) ? "\xe2\x9c\x93 "
                                        : (tst == 3) ? "\xe2\x9c\x97 " : "";
                        ImGui::PushStyleColor(ImGuiCol_Text, c);
                        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 320.f);
                        ImGui::Text("%s%s", pfx, tmsg.c_str());
                        ImGui::PopTextWrapPos();
                        ImGui::PopStyleColor();
                    }
                }

                ImGui::Dummy({0.f, 2.f});
                ImGui::SetCursorPosX(lx);
                bool vis = acfg.vision;
                if (ImGui::Checkbox("Send snapshot images to the model (vision)", &vis)) {
                    acfg.vision = vis; agent_set_config(acfg);
                }
            }

            ImGui::Dummy({0.f, 16.f});
            ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x + 8.f);
            if (ui_btn("Close", false, false)) {
                state.show_settings_modal = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::Dummy({0.f, 8.f});

            ImGui::EndPopup();
        }
        ImGui::PopStyleColor(2);
    }

    // ── Body layout ──────────────────────────────────────────────────────────
    float menubar_h  = ImGui::GetFrameHeight() + 2.f;
    float body_top   = ImGui::GetCursorPosY();
    float pipeline_h = (state.pipeline.stage != PipelineStage::Idle &&
                        state.pipeline.stage != PipelineStage::Done &&
                        state.pipeline.stage != PipelineStage::Error) ? 28.f : 0.f;
    // Reserve an extra row of layout when a windowed search is running so
    // the new search strip doesn't overlap the timeline.
    float search_h   = transcribe_search_running() ? 28.f : 0.f;
    float avail_h    = win_h - menubar_h - body_top - pipeline_h - search_h - 2.f;

    // Timeline height — user-draggable, defaults to minimum on first open
    static const float TL_MIN_H   = TL_RULER_H + 4 * TL_TRACK_H;
    float tl_h = (state.tl_h_frac > 0.f)
                  ? fmaxf(TL_MIN_H, fminf(avail_h * 0.7f, state.tl_h_frac * avail_h))
                  : TL_MIN_H;

    // Terminal strip height — user-draggable, defaults to 220 px. The agent
    // panel shares the same bottom strip (side-by-side when both are open).
    static const float TERM_MIN_H = 80.f;
    static const float TERM_DEF_H = 220.f;
    float term_h = 0.f;
    if (state.terminal_open || state.agent_panel_open) {
        term_h = (state.term_h_frac > 0.f)
                  ? fmaxf(TERM_MIN_H, fminf(avail_h * 0.6f, state.term_h_frac * avail_h))
                  : TERM_DEF_H;
    }

    float body_h = avail_h - tl_h - term_h;
    // Keep body_h above a usable minimum — steal from terminal first, then timeline.
    static const float BODY_MIN_H = 80.f;
    if (body_h < BODY_MIN_H) {
        float need = BODY_MIN_H - body_h;
        float give_term = fminf(need, fmaxf(0.f, term_h - TERM_MIN_H));
        term_h  -= give_term;
        need    -= give_term;
        float give_tl = fminf(need, fmaxf(0.f, tl_h - TL_MIN_H));
        tl_h   -= give_tl;
        body_h  = fmaxf(BODY_MIN_H, avail_h - tl_h - term_h);
    }

    // Right panel width — user-draggable, default auto
    float props_w   = (state.panel_w > 0.f)
                       ? fmaxf(200.f, fminf(win_w * 0.6f, state.panel_w))
                       : fmaxf(260.f, win_w * 0.27f);
    // ── Toolbox strip width: fits the widest label + margins (computed once) ─────
    static float s_tb_w = 0.f;
    if (s_tb_w == 0.f) {
        // "Backgrounds" is the longest label; measure it plus 24px padding
        s_tb_w = ImGui::CalcTextSize("Backgrounds").x + 24.f;
    }
    const float TB_W = s_tb_w;
    float preview_w = win_w - props_w - TB_W - 2.f;

    // ── Drag splitters ─────────────────────────────────────────────────────────
    // Runs here in the layout section — BEFORE the panel children render — so
    // the capture flag is fresh when the canvas/timeline read it this frame.
    // While a handle is hot or dragged, ui_splitter_capture() is true and the
    // canvas pick guard refuses the press: the old end-of-frame hit-test let
    // the same click fall through onto objects in the preview, and the
    // resulting simultaneous splitter-drag + canvas-drag was also the crashy
    // combination when panels were resized hard.
    {
        static bool s_drag_vsplit = false, s_drag_hsplit = false, s_drag_termsplit = false;
        ImVec2 wpos = ImGui::GetWindowPos();
        ImVec2 mpos = ImGui::GetIO().MousePos;
        float safe_avail_h = fmaxf(1.f, avail_h);   // never divide by zero on tiny windows
        bool any_hot = false;

        // Vertical splitter between preview and props
        float vborder_x = wpos.x + TB_W + preview_w + 1.f;
        bool near_v = fabsf(mpos.x - vborder_x) < 6.f &&
                      mpos.y > wpos.y + body_top &&
                      mpos.y < wpos.y + body_top + body_h;
        if (near_v || s_drag_vsplit) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            if (ImGui::IsMouseClicked(0)) s_drag_vsplit = true;
        }
        if (s_drag_vsplit) {
            state.panel_w = fmaxf(200.f, fminf(win_w * 0.6f, wpos.x + win_w - mpos.x));
            props_w   = fmaxf(200.f, fminf(win_w * 0.6f, state.panel_w));
            preview_w = win_w - props_w - TB_W - 2.f;
        }

        // Horizontal splitter between body and timeline. Terminal sits below
        // the timeline at the absolute bottom, so its height is excluded from
        // the splitter math.
        float hborder_y = wpos.y + body_top + body_h + pipeline_h;
        bool near_h = fabsf(mpos.y - hborder_y) < 6.f &&
                      mpos.x > wpos.x &&
                      mpos.x < wpos.x + win_w;
        if (near_h || s_drag_hsplit) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            if (ImGui::IsMouseClicked(0)) s_drag_hsplit = true;
        }
        if (s_drag_hsplit) {
            float new_tl_h = wpos.y + body_top + avail_h - term_h - mpos.y;
            state.tl_h_frac = fmaxf(0.1f, fminf(0.7f, new_tl_h / safe_avail_h));
        }

        // Horizontal splitter at the top edge of the terminal/agent strip
        bool near_t = false;
        if ((state.terminal_open || state.agent_panel_open) && term_h > 0.f) {
            float tborder_y = wpos.y + body_top + body_h + tl_h + pipeline_h;
            near_t = fabsf(mpos.y - tborder_y) < 6.f &&
                     mpos.x > wpos.x &&
                     mpos.x < wpos.x + win_w;
            if (near_t || s_drag_termsplit) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                if (ImGui::IsMouseClicked(0)) s_drag_termsplit = true;
            }
            if (s_drag_termsplit) {
                float new_term_h = wpos.y + body_top + avail_h - mpos.y;
                state.term_h_frac = fmaxf(0.f, fminf(0.6f, new_term_h / safe_avail_h));
            }
        }

        if (ImGui::IsMouseReleased(0))
            s_drag_vsplit = s_drag_hsplit = s_drag_termsplit = false;

        any_hot = near_v || near_h || near_t ||
                  s_drag_vsplit || s_drag_hsplit || s_drag_termsplit;
        ui_set_splitter_capture(any_hot);
    }

    // ── Toolbox strip (left rail) ──────────────────────────────────────────────
    ImGui::SetCursorPos({0.f, body_top});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(13, 13, 18, 255));
    ImGui::PushStyleColor(ImGuiCol_Border,  Col::line);
    if (ImGui::BeginChild("##toolbox_strip", {TB_W, body_h}, ImGuiChildFlags_Borders)) {
        ImDrawList* tdl = ImGui::GetWindowDrawList();
        ImVec2      sp  = ImGui::GetWindowPos();
        const float PAD_X  = 8.f;
        const float BTN_W  = TB_W - PAD_X * 2.f;
        const float BTN_H  = 26.f;
        const float BX     = sp.x + PAD_X;
        float       BY     = sp.y + 14.f;

        struct BtnDef { const char* id; PanelView lib_view; ImU32 accent; bool sep_before; bool action; };
        static const BtnDef btns[] = {
            { "Bin",         PanelView::LibBin,  IM_COL32(220, 200, 120, 255), false, false },
            { "Backgrounds", PanelView::LibBG,   IM_COL32(180,  60, 160, 255), true,  false },
            { "Text",        PanelView::LibText, IM_COL32( 80, 140, 220, 255), false, false },
            { "Lyric",       PanelView::LibLyric,IM_COL32(120, 180,  90, 255), false, false },
            { "Shape",       PanelView::Clip,    IM_COL32(180,  80, 220, 255), false, true  },
            { "Filters",     PanelView::LibAdj,  IM_COL32(100,  80, 200, 255), false, false },
            { "Video FX",    PanelView::LibFX,   IM_COL32(210, 110,  30, 255), true,  false },
            { "Body FX",     PanelView::LibBFX,  IM_COL32( 20, 180, 160, 255), false, false },
            { "Audio FX",    PanelView::LibAFX,  IM_COL32( 30, 200, 150, 255), false, false },
            { "Video",       PanelView::LibVID,  IM_COL32(140,  60, 220, 255), true,  false },
            { "Images",      PanelView::LibIMG,  IM_COL32(140,  60, 220, 255), false, false },
            { "Audio",       PanelView::LibAUD,  IM_COL32( 50, 180, 100, 255), false, false },
        };

        for (auto& b : btns) {
            if (b.sep_before) {
                tdl->AddLine({sp.x + 6.f, BY - 5.f}, {sp.x + TB_W - 6.f, BY - 5.f},
                             IM_COL32(50, 50, 65, 180), 1.f);
                BY += 4.f;
            }

            bool active = b.action ? false : (s_panel_view == b.lib_view);
            ImVec2 bmin = { BX, BY }, bmax = { BX + BTN_W, BY + BTN_H };
            bool hov = ImGui::IsMouseHoveringRect(bmin, bmax);
            ImU32 fill = active ? b.accent
                       : hov   ? IM_COL32(42, 42, 58, 255)
                                : IM_COL32(24, 24, 34, 255);
            tdl->AddRectFilled(bmin, bmax, fill, 4.f);
            if (active || hov)
                tdl->AddRect(bmin, bmax,
                    active ? b.accent : IM_COL32(70, 70, 90, 200), 4.f, 0, 1.2f);

            ImU32 tc = active ? IM_COL32(255,255,255,255) : IM_COL32(155,155,180,210);
            ImVec2 tsz = ImGui::CalcTextSize(b.id);
            tdl->AddText({BX + (BTN_W - tsz.x) * 0.5f, BY + (BTN_H - tsz.y) * 0.5f}, tc, b.id);

            ImGui::SetCursorScreenPos(bmin);
            ImGui::InvisibleButton(b.id, { BTN_W, BTN_H });
            if (ImGui::IsItemClicked()) {
                if (b.action) {
                    if (strcmp(b.id, "Shape") == 0) {
                        ImGui::OpenPopup("##shape_presets");
                    }
                } else {
                    s_panel_view = active ? pv_derive(state) : b.lib_view;
                }
            }

            BY += BTN_H + 6.f;
        }

        // ── Shape preset picker — opens from the "Shape" toolbox button. A
        // small popup listing the 12 baked presets; picking one drops a Shape
        // clip at the playhead on a free track (or a new top track) and opens
        // the Clip inspector. Mirrors the Background "Add Track" flow.
        if (ImGui::BeginPopup("##shape_presets")) {
            ImGui::SeparatorText("Shape Preset");
            static const ShapePreset s_all_presets[] = {
                ShapePreset::Circle, ShapePreset::Square, ShapePreset::Triangle,
                ShapePreset::Star,   ShapePreset::Heart,  ShapePreset::Polygon,
                ShapePreset::Hexagon,ShapePreset::Burst,  ShapePreset::Arrow,
                ShapePreset::Lightning, ShapePreset::Diamond, ShapePreset::Cross
            };
            for (ShapePreset sp : s_all_presets) {
                const char* nm = shape_preset_name(sp);
                if (ImGui::Selectable(nm)) {
                    float dur = 5.f;
                    Clip c;
                    c.clip_type = ClipType::Shape;
                    c.start = state.playhead;
                    c.end   = c.start + dur;
                    c.text  = nm;  // preset label (brick + inspector show it)
                    c.shape_path = shape_preset_bake(sp, {});
                    // Outline presets read better stroked than filled.
                    if (sp == ShapePreset::Lightning || sp == ShapePreset::Arrow ||
                        sp == ShapePreset::Burst) {
                        c.shape_style.fill_on = false;
                        c.shape_style.stroke_on = true;
                    }
                    int target = find_empty_track(state);
                    if (target < 0) {
                        Track t; t.name = "Shape";
                        state.tracks.insert(state.tracks.begin(), std::move(t));
                        target = 0;
                    }
                    state.tracks[target].clips.push_back(std::move(c));
                    state.selected_track = target;
                    state.selected_clip  = (int)state.tracks[target].clips.size() - 1;
                    clip_flash(state, target, state.selected_clip, /*reveal=*/true);
                    history_push(state, std::string("Add shape: ") + nm);
                    s_panel_view = PanelView::Clip;
                    s_user_nav   = false;
                }
            }
            ImGui::EndPopup();
        }

        // ── Record actions — not library views: each drops its brick at the
        // playhead and opens the panel ready to arm.
        {
            tdl->AddLine({sp.x + 6.f, BY - 5.f}, {sp.x + TB_W - 6.f, BY - 5.f},
                         IM_COL32(50, 50, 65, 180), 1.f);
            BY += 4.f;

            struct RecBtn {
                const char* lbl; const char* id;
                ImU32 accent, hov_fill, idle_fill, idle_border, idle_text;
                bool  is_video;
            };
            const RecBtn btns[] = {
                { "\xe2\x97\x8f Audio Rec", "##tb_record",
                  IM_COL32(220, 50, 50, 255), IM_COL32(58, 26, 30, 255),
                  IM_COL32(34, 18, 20, 255),  IM_COL32(150, 60, 65, 200),
                  IM_COL32(220, 140, 145, 220), false },
                { "\xe2\x97\x8f Video Rec", "##tb_vrecord",
                  IM_COL32(235, 90, 40, 255), IM_COL32(60, 34, 22, 255),
                  IM_COL32(36, 22, 16, 255),  IM_COL32(170, 95, 55, 200),
                  IM_COL32(235, 165, 120, 220), true },
            };
            for (auto& b : btns) {
                ImVec2 bmin = { BX, BY }, bmax = { BX + BTN_W, BY + BTN_H };
                bool hov   = ImGui::IsMouseHoveringRect(bmin, bmax);
                bool armed = b.is_video ? vrecorder_active() : recorder_active();
                ImU32 fill = armed ? b.accent : hov ? b.hov_fill : b.idle_fill;
                tdl->AddRectFilled(bmin, bmax, fill, 4.f);
                if (armed || hov)
                    tdl->AddRect(bmin, bmax, armed ? b.accent : b.idle_border,
                                 4.f, 0, 1.2f);
                ImU32 tc = armed ? IM_COL32(255,255,255,255) : b.idle_text;
                ImVec2 tsz = ImGui::CalcTextSize(b.lbl);
                tdl->AddText({BX + (BTN_W - tsz.x) * 0.5f,
                              BY + (BTN_H - tsz.y) * 0.5f}, tc, b.lbl);
                ImGui::SetCursorScreenPos(bmin);
                ImGui::InvisibleButton(b.id, { BTN_W, BTN_H });
                if (ImGui::IsItemClicked() && !armed) {
                    if (b.is_video) add_video_record_brick(state);
                    else            add_record_brick(state);
                    s_panel_view = PanelView::Clip;
                    s_user_nav   = false;
                }
                BY += BTN_H + 6.f;
            }
            // Capture IMG brick — a camera brick in photo mode: snaps a single
            // still from the webcam (same camera panel as Video Rec). Sits right
            // below the Video Rec pill.
            {
                ImVec2 bmin = { BX, BY }, bmax = { BX + BTN_W, BY + BTN_H };
                bool hov = ImGui::IsMouseHoveringRect(bmin, bmax);
                tdl->AddRectFilled(bmin, bmax,
                                   hov ? IM_COL32(22, 40, 60, 255) : IM_COL32(14, 26, 38, 255), 4.f);
                if (hov) tdl->AddRect(bmin, bmax, IM_COL32(70, 150, 235, 200), 4.f, 0, 1.2f);
                const char* lbl = "\xe2\x97\x8f Capture IMG";
                ImVec2 tsz = ImGui::CalcTextSize(lbl);
                tdl->AddText({BX + (BTN_W - tsz.x) * 0.5f, BY + (BTN_H - tsz.y) * 0.5f},
                             IM_COL32(140, 195, 245, 220), lbl);
                ImGui::SetCursorScreenPos(bmin);
                ImGui::InvisibleButton("##tb_capimg", { BTN_W, BTN_H });
                if (ImGui::IsItemClicked()) {
                    add_photo_capture_brick(state);
                    s_panel_view = PanelView::Clip;
                    s_user_nav   = false;
                }
                BY += BTN_H + 6.f;
            }
            // Audio Bus brick — drops on a new top track and submixes the
            // tracks below it (gain + FX on the grouped audio).
            {
                ImVec2 bmin = { BX, BY }, bmax = { BX + BTN_W, BY + BTN_H };
                bool hov = ImGui::IsMouseHoveringRect(bmin, bmax);
                tdl->AddRectFilled(bmin, bmax,
                                   hov ? IM_COL32(20, 46, 42, 255) : IM_COL32(14, 30, 26, 255), 4.f);
                if (hov) tdl->AddRect(bmin, bmax, IM_COL32(30, 170, 135, 200), 4.f, 0, 1.2f);
                const char* lbl = "Audio Bus";
                ImVec2 tsz = ImGui::CalcTextSize(lbl);
                tdl->AddText({BX + (BTN_W - tsz.x) * 0.5f, BY + (BTN_H - tsz.y) * 0.5f},
                             IM_COL32(120, 215, 185, 220), lbl);
                ImGui::SetCursorScreenPos(bmin);
                ImGui::InvisibleButton("##tb_bus", { BTN_W, BTN_H });
                if (ImGui::IsItemClicked()) {
                    add_bus_brick(state);
                    s_panel_view = PanelView::Clip;
                    s_user_nav   = false;
                }
                BY += BTN_H + 6.f;
            }
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);

    // ── Preview ───────────────────────────────────────────────────────────────
    ImGui::SameLine(0.f, 1.f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Col::bg);
    ImGui::PushStyleColor(ImGuiCol_Border,  Col::line);
    if (ImGui::BeginChild("##preview_zone", {preview_w, body_h}, ImGuiChildFlags_Borders)) {
        float aw = ImGui::GetContentRegionAvail().x;
        float ah = ImGui::GetContentRegionAvail().y;

        float asp_w = 9.f, asp_h = 16.f;
        if (state.format == OutputFormat::Horizontal) { asp_w = 16.f; asp_h = 9.f; }
        else if (state.format == OutputFormat::Square) { asp_w = 1.f; asp_h = 1.f; }
        float sw, sh;
        if (aw / ah > asp_w / asp_h) { sh = ah; sw = sh * asp_w / asp_h; }
        else                          { sw = aw; sh = sw * asp_h / asp_w; }
        float ox = roundf((aw - sw) * 0.5f);
        float oy = roundf((ah - sh) * 0.5f);
        ImGui::SetCursorPos({ox, oy});
        ImVec2 stage_p = ImGui::GetCursorScreenPos();
        ImGui::Dummy({sw, sh});
        draw_preview(state, stage_p, sw, sh);

        // ── Master meter (View ▸ Master meter) ───────────────────────────────
        // Compact strip on the preview's right edge: L/R sample-peak bars
        // (-60..0 dBFS), a clip light, and momentary / integrated LUFS.
        // Master-only by design; integrated resets each time playback starts.
        if (state.show_master_meter) {
            LoudnessSnapshot ms = audio_master_meter();
            ImDrawList* mdl = ImGui::GetWindowDrawList();
            const float MW = 74.f, MH = fminf(sh - 16.f, 220.f);
            ImVec2 mp0{stage_p.x + sw - MW - 8.f, stage_p.y + 8.f};
            ImVec2 mp1{mp0.x + MW, mp0.y + MH};
            mdl->AddRectFilled(mp0, mp1, IM_COL32(10, 10, 16, 215), 6.f);
            mdl->AddRect(mp0, mp1, IM_COL32(70, 70, 90, 200), 6.f);
            auto db_of    = [](float lin) { return lin > 1e-6f ? 20.f * log10f(lin) : -120.f; };
            auto bar_frac = [](float db)  { return fmaxf(0.f, fminf(1.f, (db + 60.f) / 60.f)); };
            float bar_top = mp0.y + 22.f, bar_bot = mp1.y - 40.f;
            float bx = mp0.x + 12.f;
            for (int ch = 0; ch < 2; ++ch) {
                float db = db_of(ch == 0 ? ms.peak_l : ms.peak_r);
                float f  = bar_frac(db);
                float x0 = bx + ch * 16.f, x1 = x0 + 10.f;
                mdl->AddRectFilled({x0, bar_top}, {x1, bar_bot}, IM_COL32(30, 32, 44, 255), 2.f);
                float fy = bar_bot - (bar_bot - bar_top) * f;
                ImU32 bc = db > -3.f  ? IM_COL32(235, 70, 70, 255)
                         : db > -12.f ? IM_COL32(235, 200, 70, 255)
                                      : IM_COL32(80, 210, 120, 255);
                if (f > 0.f) mdl->AddRectFilled({x0, fy}, {x1, bar_bot}, bc, 2.f);
            }
            ImU32 clip_c = ms.clipped ? IM_COL32(255, 60, 60, 255) : IM_COL32(60, 60, 74, 255);
            mdl->AddCircleFilled({mp0.x + MW * 0.5f + 18.f, mp0.y + 13.f}, 4.f, clip_c);
            mdl->AddText({mp0.x + 10.f, mp0.y + 6.f}, IM_COL32(180, 185, 200, 230), "PEAK");
            char l1[32], l2[32];
            if (ms.momentary_lufs > -100.f) snprintf(l1, sizeof(l1), "M %+5.1f", ms.momentary_lufs);
            else                            snprintf(l1, sizeof(l1), "M   --");
            if (ms.integrated_lufs > -100.f) snprintf(l2, sizeof(l2), "I %+5.1f", ms.integrated_lufs);
            else                             snprintf(l2, sizeof(l2), "I   --");
            mdl->AddText({mp0.x + 10.f, mp1.y - 34.f}, IM_COL32(210, 215, 235, 255), l1);
            mdl->AddText({mp0.x + 10.f, mp1.y - 18.f}, IM_COL32(160, 200, 255, 255), l2);
            mdl->AddText({mp0.x + MW - 38.f, mp1.y - 26.f}, IM_COL32(120, 125, 145, 200), "LUFS");
        }

        // ── Busy banner ───────────────────────────────────────────────────────
        // The hover transport pill is gone (it cluttered the preview and ate
        // clicks) — playback lives in the timeline transport. Long-running ops
        // still surface here so "why is it silent/blank" is always answered.
        {
            bool busy = audio_loading() || proxy_is_generating() || state.extract_running;
            if (busy) {
                const char* st = audio_loading()       ? "loading audio…"
                               : proxy_is_generating() ? "building preview…"
                                                       : "extracting…";
                ui_canvas_progress_banner(ImGui::GetWindowDrawList(), stage_p, sw, sh,
                                          st, -1.f, IM_COL32(120, 170, 255, 255));
            }
        }

        // ── Agent activity overlay (cute notifications) ───────────────────────
        // Visible while there's recent activity, then fades out. New entries
        // wake it back up. Slides in from the right edge on appear.
        {
            // Detect activity: log size changed, or top entry changed in-place.
            static double      s_last_change = -1e9;
            static size_t      s_prev_size   = 0;
            static std::string s_prev_top;

            std::string cur_top;
            if (!state.agent_log.empty()) {
                cur_top  = state.agent_log.front().method;
                cur_top += '\x01';
                cur_top += state.agent_log.front().detail;
            }
            if (state.agent_log.size() != s_prev_size || cur_top != s_prev_top) {
                s_last_change = ImGui::GetTime();
                s_prev_size   = state.agent_log.size();
                s_prev_top    = cur_top;
            }

            const double SHOW_FOR = 3.5;  // s of post-activity visibility
            double age = ImGui::GetTime() - s_last_change;
            float target = (!state.agent_log.empty() && age < SHOW_FOR) ? 1.f : 0.f;

            static float s_op = 0.f;
            float rate = (target > s_op) ? 14.f : 4.5f;  // fade in fast, out slow
            s_op += (target - s_op) * std::min(1.f, ImGui::GetIO().DeltaTime * rate);

            if (s_op > 0.02f && !state.agent_log.empty()) {
                float op = s_op;
                auto fa = [op](ImU32 c) -> ImU32 {
                    int a = (int)(((c >> 24) & 0xFF) * op + 0.5f);
                    if (a > 255) a = 255;
                    return (c & 0x00FFFFFFu) | ((uint32_t)a << 24);
                };
                // Easing for slide: ease-out cubic so it lands softly.
                float ease     = 1.f - op;
                float slide_dx = ease * ease * ease * 28.f;

                ImDrawList* adl   = ImGui::GetWindowDrawList();
                const float OP_H  = 17.f;
                const float PAD_X = 10.f, PAD_Y = 7.f;
                int   n      = (int)std::min(state.agent_log.size(), (size_t)5);
                float card_w = 215.f;
                float card_h = PAD_Y * 2.f + n * OP_H + (n - 1) * 2.f;
                float cx0    = stage_p.x + sw - card_w - 10.f + slide_dx;
                float cy0    = stage_p.y + 10.f;

                adl->AddRectFilled({cx0, cy0}, {cx0 + card_w, cy0 + card_h},
                                   fa(IM_COL32(0x13, 0x14, 0x1f, 210)), 7.f);
                adl->AddRect({cx0, cy0}, {cx0 + card_w, cy0 + card_h},
                             fa(IM_COL32(255, 255, 255, 18)), 7.f);

                float fsz  = ImGui::GetFontSize() * 0.82f;
                float ey   = cy0 + PAD_Y;
                for (int i = 0; i < n; ++i) {
                    const auto& op  = state.agent_log[(size_t)i];
                    float alpha     = (i == 0) ? 1.f : 0.9f - 0.15f * i;
                    int   a8        = (int)(255.f * alpha);

                    // Dot colour by method category
                    ImU32 dot;
                    if (op.method.rfind("get_", 0) == 0  ||
                        op.method.rfind("search_", 0) == 0 ||
                        op.method.rfind("find_", 0) == 0  ||
                        op.method.rfind("read_", 0) == 0)
                        dot = IM_COL32(0x7d, 0xcf, 0xff, a8);          // cyan  — reads
                    else if (op.method.find("pipeline") != std::string::npos ||
                             op.method.find("export") != std::string::npos   ||
                             op.method.find("trigger") != std::string::npos)
                        dot = IM_COL32(0xe0, 0xaf, 0x68, a8);          // amber — processing
                    else if (op.method.rfind("delete_", 0) == 0 ||
                             op.method.rfind("remove_", 0) == 0 ||
                             op.method.rfind("cut_", 0) == 0)
                        dot = IM_COL32(0xf7, 0x76, 0x8e, a8);          // red   — destructive
                    else
                        dot = IM_COL32(0x9e, 0xce, 0x6a, a8);          // green — writes

                    float mid_y = ey + OP_H * 0.5f;
                    adl->AddCircleFilled({cx0 + PAD_X + 4.f, mid_y}, 3.f, fa(dot));

                    // Label: "method name  detail"
                    std::string label = op.method;
                    for (char& c : label) if (c == '_') c = ' ';
                    if (!op.detail.empty()) label += "  " + op.detail;
                    // Truncate to fit card width
                    while (label.size() > 1) {
                        float tw = ImGui::GetFont()->CalcTextSizeA(fsz, FLT_MAX, -1.f, label.c_str()).x;
                        if (tw <= card_w - PAD_X * 2.f - 14.f) break;
                        label.resize(label.size() - 1);
                    }

                    adl->AddText(ImGui::GetFont(), fsz,
                                 {cx0 + PAD_X + 12.f, mid_y - fsz * 0.5f},
                                 fa(IM_COL32(0xc0, 0xca, 0xf5, a8)), label.c_str());

                    ey += OP_H + 2.f;
                }
            }
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);

    // ── Properties panel ─────────────────────────────────────────────────────
    ImGui::SameLine(0.f, 1.f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Col::bg_soft);
    ImGui::PushStyleColor(ImGuiCol_Border,  Col::line);
    if (ImGui::BeginChild("##props_zone", {props_w, body_h}, ImGuiChildFlags_Borders)) {
        // ── Resolve selected clip type ────────────────────────────────────────
        ClipType sel_ct = ClipType::Text;
        bool has_sel = (state.selected_track >= 0 &&
            state.selected_track < (int)state.tracks.size() &&
            state.selected_clip  >= 0 &&
            state.selected_clip  < (int)state.tracks[state.selected_track].clips.size());
        if (has_sel)
            sel_ct = state.tracks[state.selected_track].clips[state.selected_clip].clip_type;
        bool is_text_like = has_sel && (sel_ct == ClipType::Text ||
                                        sel_ct == ClipType::Lyrics ||
                                        sel_ct == ClipType::Subtitle);

        // ── Auto-switch panel on selection change ─────────────────────────────
        // s_switch_tab is a one-shot flag: fires SetSelected once to move the tab
        // bar, then clears.  Never set every frame — that fights user tab clicks.
        static bool s_switch_tab = false;
        {
            static int s_last_sel_track = -1, s_last_sel_clip = -1;
            int st = state.selected_track, sc = state.selected_clip;
            if (s_panel_request >= 0) {
                // A timeline lane click already moved the selection to the host
                // clip and asked for a specific view. Honour it and absorb the
                // selection change so the derive branch below doesn't clobber it.
                // The request is NOT cleared here — it's re-applied once more
                // AFTER the tab bar (see post-tab-bar force) because ImGui only
                // resolves the target tab's SetSelected after the earlier Clip /
                // Typography tab bodies have already re-asserted their own view.
                PanelView prev = s_panel_view;
                s_panel_view = (PanelView)s_panel_request;
                s_user_nav = false;
                if (s_panel_view != prev) s_switch_tab = true;
                s_last_sel_track = st; s_last_sel_clip = sc;
            } else if (st != s_last_sel_track || sc != s_last_sel_clip) {
                PanelView prev = s_panel_view;
                if (st >= 0 && sc >= 0) {
                    PanelView derived = pv_derive(state);
                    if (pv_is_override(derived) || !s_user_nav)
                        s_panel_view = derived;
                } else {
                    s_panel_view = PanelView::Project;
                    s_user_nav   = false;
                }
                if (s_panel_view != prev) s_switch_tab = true;
                s_last_sel_track = st; s_last_sel_clip = sc;
            }
        }

        // ── Tab bar ───────────────────────────────────────────────────────────
        bool show_clip_tabs = has_sel && !pv_is_override(s_panel_view) && !pv_is_lib(s_panel_view);
        bool show_tab_bar   = has_sel && !pv_is_lib(s_panel_view) && !pv_is_override(s_panel_view);

        if (show_tab_bar) {
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {12.f, 6.f});
            ImGui::PushStyleColor(ImGuiCol_Tab,       Col::bg_soft);
            ImGui::PushStyleColor(ImGuiCol_TabActive, Col::line);

            // Consume the one-shot switch flag before building the tab bar.
            // Pass SetSelected only on the frame we want to programmatically
            // change tabs — never every frame, to avoid fighting user clicks.
            bool do_switch = s_switch_tab;
            s_switch_tab = false;
            // Snapshot the view we want active BEFORE rendering any tab item.
            // Each tab body sets s_panel_view live when its tab is open, so a tab
            // rendered earlier in the list (Clip) would clobber s_panel_view before
            // a later tab's (FX) tf() is evaluated — and SetSelected would never
            // reach the target. Comparing against this stable copy fixes that.
            PanelView want_view = s_panel_view;
            auto tf = [&](PanelView target) -> ImGuiTabItemFlags {
                return (do_switch && want_view == target) ? ImGuiTabItemFlags_SetSelected : 0;
            };

            // FX tabs appear only while the selected content has coupled
            // chain bricks on its track: "FX" = video chain, "Audio FX" =
            // audio chain (a video clip can carry both).
            int host_fx_ti = -1, host_fx_ci = -1;
            int host_afx_ci = -1;
            if (has_sel && state.selected_track >= 0 &&
                state.selected_track < (int)state.tracks.size()) {
                auto& cls = state.tracks[state.selected_track].clips;
                if (state.selected_clip >= 0 && state.selected_clip < (int)cls.size()) {
                    for (int k = 0; k < (int)cls.size(); ++k) {
                        const Clip& oc = cls[(size_t)k];
                        if (!oc.fx_coupled) continue;
                        if (oc.clip_type != ClipType::MultiFX &&
                            oc.clip_type != ClipType::AudioMultiFX) continue;
                        if (fx_coupled_host(state, state.selected_track, oc)
                                != state.selected_clip) continue;
                        if (oc.clip_type == ClipType::MultiFX) {
                            host_fx_ti = state.selected_track;
                            host_fx_ci = k;
                        } else {
                            host_fx_ti = state.selected_track;
                            host_afx_ci = k;
                        }
                    }
                }
            }
            if (s_panel_view == PanelView::HostFX && host_fx_ci < 0)
                s_panel_view = PanelView::Clip;   // brick decoupled/deleted
            if (s_panel_view == PanelView::HostAudioFX && host_afx_ci < 0)
                s_panel_view = PanelView::Clip;

            if (ImGui::BeginTabBar("##panel_tabs")) {
                if (show_clip_tabs) {
                    if (ImGui::BeginTabItem("Clip", nullptr, tf(PanelView::Clip)))
                        { s_panel_view = PanelView::Clip; s_user_nav = false; ImGui::EndTabItem(); }
                    if (is_text_like && ImGui::BeginTabItem("Typography", nullptr, tf(PanelView::Typography)))
                        { s_panel_view = PanelView::Typography; s_user_nav = false; ImGui::EndTabItem(); }
                    if (host_fx_ci >= 0 && ImGui::BeginTabItem("FX", nullptr, tf(PanelView::HostFX)))
                        { s_panel_view = PanelView::HostFX; s_user_nav = false; ImGui::EndTabItem(); }
                    if (host_afx_ci >= 0 && ImGui::BeginTabItem("Audio FX", nullptr, tf(PanelView::HostAudioFX)))
                        { s_panel_view = PanelView::HostAudioFX; s_user_nav = false; ImGui::EndTabItem(); }
                    // Camera bricks: face filters get their own tab.
                    {
                        bool cam_sel = state.selected_track >= 0 &&
                            state.selected_track < (int)state.tracks.size() &&
                            state.selected_clip >= 0 &&
                            state.selected_clip < (int)state.tracks[state.selected_track].clips.size() &&
                            state.tracks[state.selected_track].clips[state.selected_clip]
                                .clip_type == ClipType::VideoRecord &&
                            !state.tracks[state.selected_track].clips[state.selected_clip].rec_photo;
                        if (cam_sel && ImGui::BeginTabItem("Filters", nullptr, tf(PanelView::Filters)))
                            { s_panel_view = PanelView::Filters; s_user_nav = false; ImGui::EndTabItem(); }
                    }
                }
                if (ImGui::BeginTabItem("History", nullptr, tf(PanelView::History)))
                    { s_panel_view = PanelView::History; s_user_nav = true; ImGui::EndTabItem(); }
                ImGui::EndTabBar();
            }
            s_host_fx_ti  = host_fx_ti;
            s_host_fx_ci  = host_fx_ci;
            s_host_afx_ci = host_afx_ci;

            // Post-tab-bar force: a pending lane-click request wins over whatever
            // the tab bodies just set s_panel_view to. SetSelected (s_switch_tab)
            // moves the visible tab for next frame; this guarantees the CONTENT
            // below matches the requested FX view from the very first frame.
            if (s_panel_request >= 0) {
                PanelView want = (PanelView)s_panel_request;
                bool ok = (want == PanelView::HostFX      && host_fx_ci  >= 0) ||
                          (want == PanelView::HostAudioFX && host_afx_ci >= 0) ||
                          (want != PanelView::HostFX && want != PanelView::HostAudioFX);
                if (ok) s_panel_view = want;
                s_panel_request = -1;
            }

            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar();
        }
        // Drop a stale request if the tab bar wasn't shown this frame (no
        // selection / library / override view) so it can't leak to a later frame.
        s_panel_request = -1;

        // ── Panel content ─────────────────────────────────────────────────────
        ImGui::BeginChild("##panel_scroll", {0.f, 0.f});
        ImGui::SetCursorPosX(8.f);
        float pw = props_w - 16.f;

        switch (s_panel_view) {
            case PanelView::Clip:        panel_clip(state, pw);                  break;
            case PanelView::Filters:     panel_face_filters(state, pw);          break;
            case PanelView::Typography:  panel_typography(state, pw);            break;
            case PanelView::HostAudioFX:
                panel_audio_multifx_for(state, pw, s_host_fx_ti, s_host_afx_ci);
                break;
            case PanelView::OverrideAudioMultiFX:
                panel_audio_multifx(state, pw);
                break;
            case PanelView::HostFX: {
                // Coupled chain of the selected content + the way out.
                panel_multifx_for(state, pw, s_host_fx_ti, s_host_fx_ci);
                if (s_host_fx_ti >= 0 && s_host_fx_ci >= 0 &&
                    s_host_fx_ti < (int)state.tracks.size() &&
                    s_host_fx_ci < (int)state.tracks[s_host_fx_ti].clips.size()) {
                    Clip& bk = state.tracks[s_host_fx_ti].clips[(size_t)s_host_fx_ci];
                    if (bk.clip_type == ClipType::MultiFX && bk.fx_coupled) {
                        ImGui::Dummy({0.f, 8.f});
                        ImGui::SetCursorPosX(8.f);
                        if (ui_btn("Decouple Multi-FX brick", false, true)) {
                            int host = fx_coupled_host(state, s_host_fx_ti, bk);
                            float blen = bk.end - bk.start;
                            bk.fx_coupled = false;
                            bk.fx_host_sid.clear();
                            if (host >= 0) {
                                const Clip& hc = state.tracks[s_host_fx_ti]
                                                     .clips[(size_t)host];
                                bk.start = hc.end;
                                bk.end   = hc.end + fminf(2.f, fmaxf(0.5f, blen));
                            }
                            history_push(state, "Decouple Multi-FX brick");
                            s_panel_view = PanelView::Clip;
                        }
                    }
                }
                break;
            }
            case PanelView::Project:     panel_project(state, pw);               break;
            case PanelView::History:     panel_history(state, pw);               break;
            case PanelView::LibBG:           panel_background(state, pw);            break;
            case PanelView::LibText:         panel_text_library(state, pw);          break;
            case PanelView::LibLyric:        panel_lyric_library(state, pw);         break;
            case PanelView::LibFX:           panel_fx_creative(state, pw);           break;
            case PanelView::LibAdj:          panel_adjustment_library(state, pw);    break;
            case PanelView::LibBFX:          panel_body_fx_library(state, pw);       break;
            case PanelView::LibAFX:          panel_fx_audio(state, pw);              break;
            case PanelView::LibVID:          panel_media_browser(state, pw, true);   break;
            case PanelView::LibIMG:          panel_media_browser(state, pw, false);  break;
            case PanelView::LibAUD:          panel_audio_browser(state, pw);         break;
            case PanelView::LibBin:          panel_bin(state, pw);                   break;
            case PanelView::OverrideFX:      panel_fx_clip(state, pw);               break;
            case PanelView::OverrideAdj:     panel_adjustment(state, pw);            break;
            case PanelView::OverrideBG:      panel_background(state, pw, true);      break;
            case PanelView::OverrideAudioFX: panel_audio_fx_clip(state, pw);         break;
            case PanelView::OverrideMultiFX: panel_multifx(state, pw);               break;
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);

    // (Drag splitters now run in the body-layout section above, BEFORE the
    // panel children render, so the canvas/timeline input guards can see the
    // splitter capture flag in the same frame — see ui_splitter_capture().)

    // ── Quit confirm: unsaved changes ─────────────────────────────────────────
    if (state.quit_prompt) {
        ImGui::OpenPopup("Save changes?##quit");
        state.quit_prompt = false;   // popup owns the flow from here
    }
    {
        ImVec2 dc_ = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos({dc_.x * 0.5f, dc_.y * 0.42f}, ImGuiCond_Appearing, {0.5f, 0.5f});
        if (ImGui::BeginPopupModal("Save changes?##quit", nullptr,
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
            std::string name = state.project_path.empty()
                ? "Untitled"
                : fs::path(state.project_path).stem().string();
            ImGui::Text("Save changes to \"%s\" before closing?", name.c_str());
            ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
            ImGui::TextUnformatted("Your edits since the last save will be lost otherwise.");
            ImGui::PopStyleColor();
            ImGui::Dummy({0.f, 8.f});
            if (ImGui::Button("Save", {110.f, 0.f})) {
                std::string p = state.project_path;
                if (p.empty())
                    p = filepicker_save("Save project", "PMS Project", "*.pms",
                                        project_save_default(state).c_str());
                if (!p.empty() && project_save(state, p)) {
                    state.project_path = p;
                    recent_projects_push(p);
                    recovery_clear();
                    mark_project_saved(state, p);
                    state.quit_confirmed = true;
                    ImGui::CloseCurrentPopup();
                }
                // picker cancelled / save failed → stay open, nothing lost
            }
            ImGui::SameLine(0.f, 8.f);
            if (ImGui::Button("Don't Save", {110.f, 0.f})) {
                state.quit_confirmed = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine(0.f, 8.f);
            if (ImGui::Button("Cancel", {110.f, 0.f}) ||
                ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    // ── Pipeline strip ────────────────────────────────────────────────────────
    if (pipeline_h > 0.f) {
        ImGui::SetCursorPos({0.f, ImGui::GetCursorPosY()});
        draw_pipeline_strip(state, win_w);
    }
    // Surface agent-driven find_and_add_clip / search_transcript runs in the
    // same strip lane so the human can see what's being scanned.
    draw_search_strip(win_w);

    // ── Timeline panel ────────────────────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Col::bg);
    ImGui::PushStyleColor(ImGuiCol_Border,  Col::line);
    if (ImGui::BeginChild("##tl_zone", {win_w, tl_h}, ImGuiChildFlags_Borders,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        // Header bar
        ImDrawList* tl_dl = ImGui::GetWindowDrawList();
        ImVec2 hdr_tl = ImGui::GetCursorScreenPos();
        float  hdr_w  = ImGui::GetContentRegionAvail().x;
        constexpr float HDR_H = 22.f;
        tl_dl->AddRectFilled(hdr_tl, {hdr_tl.x+hdr_w, hdr_tl.y+HDR_H}, to_u32(Col::bg_soft));
        tl_dl->AddLine({hdr_tl.x, hdr_tl.y+HDR_H}, {hdr_tl.x+hdr_w, hdr_tl.y+HDR_H}, to_u32(Col::line));
        ImGui::PushFont(g_font_bold);
        tl_dl->AddText({hdr_tl.x+8.f, hdr_tl.y+4.f}, to_u32(Col::muted), "TIMELINE");
        ImGui::PopFont();

        // Loop + marker controls — always visible here (the transport pill's
        // loop button auto-hides, so the timeline is their discoverable home).
        {
            ImGui::SetCursorScreenPos({hdr_tl.x + 86.f, hdr_tl.y + 2.f});
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {6.f, 2.f});
            // Loop toggle — green when armed.
            bool on = state.loop_play;
            ImVec4 loop_btn  = on ? ImVec4(0.16f,0.59f,0.35f,1.f) : Col::bg_soft;
            ImVec4 loop_btnh = on ? ImVec4(0.20f,0.69f,0.41f,1.f) : Col::bg_soft_hov;
            ImGui::PushStyleColor(ImGuiCol_Button,        loop_btn);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, loop_btnh);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  loop_btnh);
            if (ImGui::SmallButton("Loop##tlloop")) state.loop_play = !state.loop_play;
            ImGui::PopStyleColor(3);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Loop playback over the brace region (whole timeline if none set)");
            ImGui::SameLine(0.f, 8.f);
            ImGui::PushStyleColor(ImGuiCol_Button,        Col::bg_soft);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Col::bg_soft_hov);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  Col::bg_soft_hov);
            if (ImGui::SmallButton("+Mark##tlmark")) marker_add(state, state.playhead);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drop a marker at the playhead (M)");
            ImGui::SameLine(0.f, 6.f);
            if (ImGui::SmallButton("<##tlmprev")) marker_jump(state, -1);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Jump to previous marker ([)");
            ImGui::SameLine(0.f, 2.f);
            if (ImGui::SmallButton(">##tlmnext")) marker_jump(state, +1);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Jump to next marker (])");
            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar();
        }

        // Zoom controls in header. The readout is the position within the
        // usable zoom range: 0% = fully zoomed out (whole project fits),
        // 100% = maximum zoom. Log-mapped so steps feel uniform — raw px/s is
        // meaningless to a person ("1347%" on an empty project).
        {
            const float zmax = 4000.f;
            float zmin = fmaxf(1.f, state.tl_zoom_min);
            float pct  = 0.f;
            if (zmax > zmin * 1.001f)
                pct = 100.f * logf(fmaxf(state.tl_zoom, zmin) / zmin) / logf(zmax / zmin);
            pct = fmaxf(0.f, fminf(100.f, pct));
            char zbuf[20]; snprintf(zbuf, sizeof(zbuf), "%.0f%%", pct);
            float zx = hdr_tl.x + hdr_w - 120.f;
            ImGui::SetCursorScreenPos({zx, hdr_tl.y+2.f});
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {4.f, 2.f});
            ImGui::PushStyleColor(ImGuiCol_Button,        Col::bg_soft);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Col::bg_soft_hov);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  Col::bg_soft_hov);
            if (ImGui::SmallButton("-##zout")) state.tl_zoom = fmaxf(state.tl_zoom*0.8f, state.tl_zoom_min);
            ImGui::SameLine(0.f,4.f);
            ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
            ImGui::SetNextItemWidth(48.f);
            ImGui::TextUnformatted(zbuf);
            ImGui::PopStyleColor();
            ImGui::SameLine(0.f,4.f);
            if (ImGui::SmallButton("+##zin"))  state.tl_zoom = fminf(state.tl_zoom*1.25f, 4000.f);
            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar();
        }

        // Advance cursor past header, then draw timeline
        ImGui::SetCursorScreenPos({hdr_tl.x, hdr_tl.y + HDR_H});
        ImVec2 tl_origin = ImGui::GetCursorScreenPos();
        float  tl_w      = ImGui::GetContentRegionAvail().x;
        float  tl_h_real = ImGui::GetContentRegionAvail().y;
        ImGui::Dummy({tl_w, tl_h_real});
        draw_timeline(state, tl_origin, tl_w, tl_h_real);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);

    // ── Terminal / Agent strip ───────────────────────────────────────────────
    // Lives at the absolute bottom (below the timeline) so the editing surface
    // sits directly under the preview canvas without an unrelated strip
    // wedged between them. Terminal and Agent share the strip 50/50 when both
    // are open; either alone takes the full width.
    if ((state.terminal_open || state.agent_panel_open) && term_h > 0.f) {
        // Single occupancy: the toggle buttons enforce that at most one of
        // the two panels is open, and each gets the full strip width.
        if (state.agent_panel_open) {
            float agent_w = win_w;
            // Tall enough for the input row + padding — content must never
            // exceed this, since the zone deliberately can't scroll. Grows to
            // fit the drop-staging chip tray when files are staged.
            float inp_h = agent_input_height();
            float log_h = term_h - inp_h;
            ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0x16, 0x16, 0x20, 255));
            ImGui::PushStyleColor(ImGuiCol_Border,  Col::line);
            if (ImGui::BeginChild("##agent_log_zone", {agent_w, log_h},
                                  ImGuiChildFlags_Borders)) {
                draw_agent_log(state, agent_w - 2.f, log_h - 2.f);
            }
            ImGui::EndChild();
            ImGui::PopStyleColor(2);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0x16, 0x16, 0x20, 255));
            // Border accents toward the agent magenta when the input is the drop
            // target (focused), brightening on a brief flash right after a drop.
            float hl = agent_drop_highlight();
            ImVec4 inp_border = Col::line;
            if (hl > 0.f) {
                const ImVec4 acc = {0.73f, 0.60f, 0.97f, 1.0f};  // agent magenta
                inp_border.x = Col::line.x + (acc.x - Col::line.x) * hl;
                inp_border.y = Col::line.y + (acc.y - Col::line.y) * hl;
                inp_border.z = Col::line.z + (acc.z - Col::line.z) * hl;
                inp_border.w = Col::line.w + (0.85f - Col::line.w) * hl;
            }
            ImGui::PushStyleColor(ImGuiCol_Border, inp_border);
            if (ImGui::BeginChild("##agent_inp_zone", {agent_w, inp_h},
                                  ImGuiChildFlags_Borders,
                                  ImGuiWindowFlags_NoScrollbar |
                                  ImGuiWindowFlags_NoScrollWithMouse)) {
                draw_agent_input(state, agent_w - 2.f);
            }
            ImGui::EndChild();
            ImGui::PopStyleColor(2);
        } else {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0x1a, 0x1b, 0x26, 255));
            ImGui::PushStyleColor(ImGuiCol_Border,  Col::line);
            if (ImGui::BeginChild("##term_zone", {win_w, term_h}, ImGuiChildFlags_Borders)) {
                draw_terminal_panel(state, win_w - 2.f, term_h - 2.f);
            }
            ImGui::EndChild();
            ImGui::PopStyleColor(2);
        }
    }

    // ── Tutorial floating panel ───────────────────────────────────────────────
    if (state.show_tutorial && state.tutorial_step < 5) {
        // Auto-advance conditions
        if (state.tutorial_step == 0) {
            bool has_video = false;
            for (auto& tr : state.tracks)
                for (auto& cl : tr.clips)
                    if (clip_is_videolike_type(cl.clip_type)) { has_video = true; break; }
            if (has_video) state.tutorial_step = 1;
        }
        if (state.tutorial_step == 2 && !state.beats.empty())
            state.tutorial_step = 3;
        if (state.tutorial_step == 3) {
            for (auto& tr : state.tracks)
                for (auto& cl : tr.clips)
                    if (cl.bg_remove_status == BgRemoveStatus::Ready)
                        state.tutorial_step = 4;
        }

        struct TutStep { const char* title; const char* body; bool manual_next; };
        static const TutStep steps[5] = {
            { "1 / 5 — Drop footage",
              "Drag a video file onto the timeline.\nIt will appear as a clip on a new track.",
              false },
            { "2 / 5 — Trim your clip",
              "Drag the edges of a clip to trim it.\nPress S to split at the playhead.\nPress Next when you're happy with the length.",
              true },
            { "3 / 5 — Sync to beats",
              "Click the Detect Beats button in the ML\nProcessing bar. Once done, beat markers\nappear on the timeline ruler — hold Shift\nand drag clips to snap them to beats.",
              false },
            { "4 / 5 — Remove background",
              "Select a video clip, open the Clip tab,\nscroll to Remove Background and click Run.\nThe mask streams in as frames process.",
              false },
            { "5 / 5 — Export",
              "Click the Export button in the top-right\ncorner, choose your format, and render\nto MP4. That's it — you're done!",
              true },
        };

        const TutStep& step = steps[state.tutorial_step];
        float panel_w = 280.f;
        float margin  = 24.f;
        ImGui::SetNextWindowPos({win_w - panel_w - margin, menubar_h + margin},
                                ImGuiCond_Always);
        ImGui::SetNextWindowSize({panel_w, 0.f}, ImGuiCond_Always);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, to_u32(Col::bg));
        ImGui::PushStyleColor(ImGuiCol_Border,   to_u32(Col::line));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {14.f, 12.f});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {6.f, 6.f});

        ImGui::Begin("##tutorial_panel", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBringToFrontOnFocus);

        ImGui::PushFont(g_font_bold);
        ImGui::TextUnformatted("Getting Started");
        ImGui::PopFont();
        ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
        ImGui::TextUnformatted(step.title);
        ImGui::PopStyleColor();
        ImGui::Dummy({0.f, 4.f});
        ImGui::TextWrapped("%s", step.body);
        ImGui::Dummy({0.f, 8.f});

        float btn_w = (panel_w - 28.f - 6.f) * 0.5f;
        ImGui::PushStyleColor(ImGuiCol_Text, Col::dim);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0.f, 3.f});
        ImGui::SetNextItemWidth(btn_w);
        if (ImGui::Button("Skip tutorial##tut_skip", {btn_w, 0.f}))
            state.show_tutorial = false;
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::SameLine(0.f, 6.f);
        ImGui::PushStyleColor(ImGuiCol_Button,        Col::fg);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Col::bg_soft_hov);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  Col::line);
        ImGui::PushStyleColor(ImGuiCol_Text,          Col::bg);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0.f, 3.f});
        bool last_step = (state.tutorial_step == 4);
        if (step.manual_next || last_step) {
            const char* lbl = last_step ? "Done##tut_done" : "Next##tut_next";
            if (ImGui::Button(lbl, {btn_w, 0.f})) {
                if (last_step) state.show_tutorial = false;
                else           state.tutorial_step++;
            }
        } else {
            ImGui::BeginDisabled();
            ImGui::Button("Next##tut_next_dis", {btn_w, 0.f});
            ImGui::EndDisabled();
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);

        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);
    }
}
