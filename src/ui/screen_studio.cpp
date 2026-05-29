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
#include "globals.h"
#include "render.h"
#include "history.h"
#include "blender_export.h"
#include "project.h"
#include <imgui.h>
#include <imgui_internal.h>
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
static bool s_user_nav = false; // user explicitly chose Animation/History tab

static void handle_shortcuts(AppState& state) {
    if (ImGui::IsAnyItemActive()) return;
    ImGuiIO& io = ImGui::GetIO();

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
            state.project_path = filepicker_save("Save project", "PMS Project", "*.pms");
        if (!state.project_path.empty()) project_save(state, state.project_path);
        return;
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S)) {
        std::string p = filepicker_save("Save project as", "PMS Project", "*.pms");
        if (!p.empty()) { state.project_path = p; project_save(state, p); }
        return;
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_N)) {
        transcribe_cancel(); history_clear();
        audio_shutdown(); audio_clips_clear(); video_close();
        state = AppState{}; state.splash_timer = 0.f;
        audio_init();
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
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
        float step = io.KeyShift ? 5.f : f_dt;
        seek_to(state, fminf(state.playhead + step, dur));
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
        float step = io.KeyShift ? 5.f : f_dt;
        seek_to(state, fmaxf(state.playhead - step, 0.f));
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Home)) { seek_to(state, 0.f);  return; }
    if (ImGui::IsKeyPressed(ImGuiKey_End))  { seek_to(state, dur);  return; }

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
            Clip right = clip; clip.end = cut; right.start = cut;
            right.in_point += (cut - clip.start) * clip.speed;
            track.clips.insert(track.clips.begin()+state.selected_clip+1, right);
            history_push(state, "Split clip");
        }
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Delete) ||
        ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
        track.clips.erase(track.clips.begin()+state.selected_clip);
        state.selected_clip = -1;
        history_push(state, "Delete clip");
        return;
    }
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
    poll_clip_beat_analysis(state);

    // Keep state.duration in sync with actual clip content every frame.
    if (!state.tracks.empty()) {
        float pe = project_end(state);
        if (pe > 0.01f) state.duration = pe;
    }

    // Handle OS drop — SRT, audio, or video.
    extern std::string g_dropped_file;
    if (!g_dropped_file.empty()) {
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
            // Dropped image: new track + 5-second clip at playhead, same as Browse
            recent_media_push(dp, MediaKind::Image);
            Track nt; nt.name = fp.stem().string();
            Clip cl;
            cl.clip_type = ClipType::Video;
            cl.text      = dp;
            cl.source_id = dp;
            cl.start     = state.playhead;
            cl.end       = cl.start + 5.f;
            nt.clips.push_back(cl);
            state.tracks.insert(state.tracks.begin(), std::move(nt));
            state.selected_track = 0;
            state.selected_clip  = 0;
            proxy_start(dp);
            int slot = slot_for_video(state, clip_slot_key(dp, cl.start), dp);
            if (slot >= 0) video_open_still(slot, proxy_still_path(dp));
            state.video_loaded = true;
            s_drop_flash_track = 0;
            s_drop_flash_t     = 0.6f;
            history_push(state, "Import image: " + fp.filename().string());
        } else if (is_audio_file(dp)) {
            bool is_vid = (ext==".mp4"||ext==".mov"||ext==".mkv"||ext==".avi"||ext==".webm");
            ClipType drop_ct = is_vid ? ClipType::Video : ClipType::Audio;

            if (s_tl_hover_track >= 0 && s_tl_hover_track < (int)state.tracks.size()) {
                add_clip_to_track(state, s_tl_hover_track, dp, drop_ct);
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

    // Per-slot video open/upgrade — handles three states:
    //   1. Already a full proxy (fps > 0)  → nothing to do
    //   2. Proxy ready but slot not open   → open proxy directly (covers split/moved clips)
    //   3. No proxy yet, slot not open     → open still as placeholder
    // Images never get an MJPEG proxy — skip them to avoid per-frame ffprobe spawns.
    for (int slot = 0; slot < MAX_VIDEO_TRACKS; ++slot) {
        const std::string& key = state.proxy_paths[slot];
        if (key.empty()) continue;
        if (video_info(slot).fps > 0.0) continue;  // already fully open

        std::string src = source_from_key(key);
        if (is_image_path(src)) continue;  // stills only — proxy never generated
        if (proxy_is_ready(src)) {
            ProxyInfo pi;
            if (!proxy_load(src, pi)) continue;
            video_open_proxy(slot, pi);
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
            // Proxy not ready yet — show the still thumbnail while it generates.
            video_open_still(slot, proxy_still_path(src));
        }
    }

    // GC slots whose clips have been deleted or moved.
    gc_video_slots(state);

    // IPC may have added new video clips — trigger proxy scan on next frame.
    if (state.proxy_scan_needed) {
        state.proxy_scan_needed = false;
        reopen_video_slots(state);
    }

    // Poll background removal and voice conversion jobs.
    bg_remove_poll(state);
    vc_poll(state);

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

        if (state.pipeline_produces_subtitles) {
            state.lyrics_edits.clear();
            load_words_cache(state);
            apply_subtitle_pipeline(state);
            save_all_srts(state);
        } else if (!state.pipeline_is_separate_only) {
            // Both mode: has words + vocals
            state.lyrics_edits.clear();
            load_words_cache(state);
            // Skip apply_subtitle_mode when typography will regenerate from words_cache immediately after
            if (!state.typo_generate_when_done)
                apply_subtitle_mode(state);
            save_all_srts(state);
        }
        // SeparateOnly: no words, skip subtitle machinery entirely

        // Add vocals stem to timeline only for explicit "Separate vocals" runs
        if (state.pipeline_is_separate_only && !state.vocals_path.empty() && fs::exists(state.vocals_path)) {
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

        if (state.typo_generate_when_done) {
            state.typo_generate_when_done = false;
            generate_typography(state);
        }

        state.pipeline_is_separate_only = false;
    }
    last_stage = state.pipeline.stage;

    // Push clip snapshots to audio system every frame.
    // The callback reads these to position audio correctly — no separate volume hack needed.
    {
        std::vector<AudioClipDesc> vdescs, adescs;
        for (auto& tr : state.tracks) {
            if (tr.muted) continue;
            for (auto& cl : tr.clips) {
                if (cl.text.empty() || cl.muted) continue;
                AudioClipDesc d;
                d.tl_start = cl.start;    d.tl_end   = cl.end;
                d.in_point = cl.in_point; d.speed    = cl.speed;
                d.volume   = cl.volume;   d.pan      = cl.pan;
                d.fade_in  = cl.fade_in;  d.fade_out = cl.fade_out;
                // Use converted audio when voice conversion is ready
                if (cl.clip_type == ClipType::Audio
                    && cl.vc_status == VcStatus::Ready
                    && !cl.vc_out_path.empty()) {
                    d.path = cl.vc_out_path;
                } else {
                    d.path = cl.text;
                }
                {
                    // Merge per-clip AudioFX with any audio FX bricks on the same track.
                    // Exclude voice_convert_on when the clip already has a converted source.
                    AudioFX combined = collect_audio_fx_for_clip(state, (int)(&tr - state.tracks.data()), cl);
                    if (cl.audio_fx.any_active()) combined = cl.audio_fx;
                    if (cl.vc_status == VcStatus::Ready) combined.voice_convert_on = false;
                    if (combined.any_active()) {
                        d.fx      = combined;
                        d.fx_hash = audio_fx_hash(combined);
                    }
                }
                if (cl.clip_type == ClipType::Video) {
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
                transcribe_cancel();
                history_clear();
                state = AppState{};
                state.splash_timer = 0.f;
                audio_shutdown(); audio_clips_clear(); audio_init();
                video_close();
            }
            if (ImGui::MenuItem("Open Project…", "Ctrl+Shift+O")) {
                std::string picked = filepicker_open("Open project", "PMS Project", "*.pms");
                if (!picked.empty()) {
                    AppState loaded;
                    if (project_load(loaded, picked)) {
                        transcribe_cancel(); history_clear();
                        audio_shutdown(); audio_clips_clear(); video_close();
                        state = std::move(loaded);
                        state.project_path = picked;
                        audio_init();
                        if (!state.audio_path.empty()) audio_load(state.audio_path.c_str());
                        reopen_video_slots(state);
                        // Ensure sources for all Audio clips
                        for (auto& tr : state.tracks)
                            for (auto& cl : tr.clips)
                                if (cl.clip_type == ClipType::Audio && !cl.text.empty())
                                    audio_source_ensure(cl.text);
                    }
                }
            }
            if (ImGui::MenuItem("Save Project", "Ctrl+S")) {
                if (state.project_path.empty())
                    state.project_path = filepicker_save("Save project", "PMS Project", "*.pms");
                if (!state.project_path.empty()) project_save(state, state.project_path);
            }
            if (ImGui::MenuItem("Save Project As…", "Ctrl+Shift+S")) {
                std::string p = filepicker_save("Save project as", "PMS Project", "*.pms");
                if (!p.empty()) { state.project_path = p; project_save(state, p); }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Import Audio / Video…", "Ctrl+O")) {
                std::string picked = filepicker_open(
                    "Import audio or video",
                    "Audio & Video", "*.wav *.mp3 *.m4a *.flac *.aac *.mp4 *.mov *.mkv");
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
                Track& t = state.tracks[state.selected_track];
                Clip& c = t.clips[state.selected_clip];
                float cut = state.playhead;
                if (cut>c.start+0.02f && cut<c.end-0.02f) {
                    Clip r=c; c.end=cut; r.start=cut;
                    r.in_point += (cut - c.start) * c.speed;
                    t.clips.insert(t.clips.begin()+state.selected_clip+1, r);
                    history_push(state, "Split clip");
                }
            }
            if (ImGui::MenuItem("Duplicate clip") && has_clip) {
                Track& t = state.tracks[state.selected_track];
                Clip dup = t.clips[state.selected_clip];
                float len = dup.end - dup.start;
                dup.start = dup.end; dup.end = dup.start+len;
                t.clips.insert(t.clips.begin()+state.selected_clip+1, dup);
                history_push(state, "Duplicate clip");
            }
            if (ImGui::MenuItem("Delete clip", "Del") && has_clip) {
                state.tracks[state.selected_track].clips.erase(
                    state.tracks[state.selected_track].clips.begin()+state.selected_clip);
                state.selected_clip=-1;
                history_push(state, "Delete clip");
            }
            if (!has_clip) ImGui::EndDisabled();
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Zoom in",  "Ctrl++")) state.tl_zoom = fminf(state.tl_zoom*1.25f, 4000.f);
            if (ImGui::MenuItem("Zoom out", "Ctrl+-")) state.tl_zoom = fmaxf(state.tl_zoom*0.8f,  20.f);
            if (ImGui::MenuItem("Fit timeline")) {
                if (state.duration > 0.f) {
                    float avail = win_w - TL_LABEL_W - 20.f;
                    state.tl_zoom   = avail / state.duration;
                    state.tl_scroll = 0.f;
                }
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
            ImGui::Separator();
            bool already = state.models_ready;
            if (already) ImGui::BeginDisabled();
            if (ImGui::MenuItem("Set Up AI Features…"))
                state.show_model_dl_modal = true;
            if (already) ImGui::EndDisabled();
            ImGui::EndMenu();
        }

        // Project + Export buttons — far right of menu bar
        {
            float btn_export_w = 80.f;
            float btn_proj_w   = 70.f;
            float avail = ImGui::GetContentRegionAvail().x;
            float total_btns = btn_proj_w + 6.f + btn_export_w;
            if (avail > total_btns)
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - total_btns);

            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {10.f, 2.f});
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
        ImGui::SetNextWindowSize({480.f, 0.f});
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


            // ── Lyric extraction models ───────────────────────────────────────
            ui_label("Lyric extraction models");
            ImGui::Dummy({0.f, 4.f});
            ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x + 8.f);
            if (state.models_ready) {
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(100, 220, 130, 255));
                ImGui::TextUnformatted("Installed");
                ImGui::PopStyleColor();
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
                ImGui::TextUnformatted("Not installed");
                ImGui::PopStyleColor();
                ImGui::SameLine(0.f, 12.f);
                if (ui_btn("Download…", false, true)) {
                    state.show_settings_modal = false;
                    state.show_model_dl_modal = true;
                    ImGui::CloseCurrentPopup();
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
    float avail_h    = win_h - menubar_h - body_top - pipeline_h - 2.f;

    // Timeline height — user-draggable, defaults to minimum on first open
    static const float TL_MIN_H = TL_RULER_H + 4 * TL_TRACK_H;
    float tl_h       = (state.tl_h_frac > 0.f)
                        ? fmaxf(TL_MIN_H, fminf(avail_h * 0.7f, state.tl_h_frac * avail_h))
                        : TL_MIN_H;
    float body_h     = avail_h - tl_h;

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

        struct BtnDef { const char* id; PanelView lib_view; ImU32 accent; bool sep_before; };
        static const BtnDef btns[] = {
            { "Backgrounds", PanelView::LibBG,   IM_COL32(180,  60, 160, 255), false },
            { "Effects",     PanelView::LibFX,   IM_COL32(210, 110,  30, 255), true  },
            { "Filters",     PanelView::LibAdj,  IM_COL32(100,  80, 200, 255), false },
            { "Body FX",     PanelView::LibBFX,  IM_COL32( 20, 180, 160, 255), false },
            { "Audio FX",    PanelView::LibAFX,  IM_COL32( 30, 200, 150, 255), false },
            { "Video",       PanelView::LibVID,  IM_COL32(140,  60, 220, 255), true  },
            { "Images",      PanelView::LibIMG,  IM_COL32(140,  60, 220, 255), false },
            { "Audio",       PanelView::LibAUD,  IM_COL32( 50, 180, 100, 255), false },
        };

        for (auto& b : btns) {
            if (b.sep_before) {
                tdl->AddLine({sp.x + 6.f, BY - 5.f}, {sp.x + TB_W - 6.f, BY - 5.f},
                             IM_COL32(50, 50, 65, 180), 1.f);
                BY += 4.f;
            }

            bool active = (s_panel_view == b.lib_view);
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
            if (ImGui::IsItemClicked())
                s_panel_view = active ? pv_derive(state) : b.lib_view;

            BY += BTN_H + 6.f;
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

        // ── Glass transport overlay ───────────────────────────────────────────
        {
            ImDrawList* dl  = ImGui::GetWindowDrawList();
            float fps_v     = tl_fps(state);
            float f_dt_v    = fps_v > 0.f ? 1.f / fps_v : 1.f / 30.f;
            float dur       = fmaxf(state.duration, 0.01f);
            bool  busy      = audio_loading() || proxy_is_generating() || state.extract_running;

            // ── Geometry ──────────────────────────────────────────────────────
            const float PILL_PAD_X = 16.f;
            const float PILL_PAD_Y = 10.f;
            const float SCRUB_H    = 4.f;   // resting height of scrubber track
            const float SCRUB_H_H  = 6.f;   // hovered height
            const float BTN_ROW_H  = 36.f;
            const float TC_ROW_H   = 18.f;  // timecode row below buttons
            const float PILL_H     = SCRUB_H + BTN_ROW_H + TC_ROW_H + PILL_PAD_Y * 2.f + 10.f;
            const float PILL_W     = fminf(sw, fmaxf(360.f, sw * 0.85f));
            const float PILL_R     = 16.f;

            float pill_x0 = stage_p.x + (sw - PILL_W) * 0.5f;
            float pill_x1 = pill_x0 + PILL_W;
            float pill_y1 = stage_p.y + sh - 14.f;
            float pill_y0 = pill_y1 - PILL_H;

            // Gradient shadow behind pill
            dl->AddRectFilledMultiColor(
                {stage_p.x, pill_y0 - 40.f}, {stage_p.x + sw, pill_y1 + 8.f},
                IM_COL32(0,0,0,0),   IM_COL32(0,0,0,0),
                IM_COL32(0,0,0,160), IM_COL32(0,0,0,160));

            // Glass pill body
            dl->AddRectFilled({pill_x0, pill_y0}, {pill_x1, pill_y1},
                              IM_COL32(18, 18, 22, 210), PILL_R);
            // Top-edge glass highlight
            dl->AddLine({pill_x0 + PILL_R, pill_y0 + 1.f},
                        {pill_x1 - PILL_R, pill_y0 + 1.f},
                        IM_COL32(255,255,255,28), 1.f);
            // Outer border
            dl->AddRect({pill_x0, pill_y0}, {pill_x1, pill_y1},
                        IM_COL32(255,255,255,22), PILL_R, 0, 1.f);

            // ── Scrubber ──────────────────────────────────────────────────────
            float scrub_margin = PILL_PAD_X + 4.f;
            float scrub_x0 = pill_x0 + scrub_margin;
            float scrub_x1 = pill_x1 - scrub_margin;
            float scrub_w  = scrub_x1 - scrub_x0;
            float scrub_cy = pill_y0 + PILL_PAD_Y + SCRUB_H * 0.5f;

            ImGui::SetCursorScreenPos({scrub_x0, scrub_cy - 10.f});
            ImGui::InvisibleButton("##scrub", {scrub_w, 20.f});
            bool scrub_hov  = ImGui::IsItemHovered();
            bool scrub_held = ImGui::IsItemActive();

            static float s_scrub_h_anim = SCRUB_H;
            float scrub_h_target = (scrub_hov || scrub_held) ? SCRUB_H_H : SCRUB_H;
            s_scrub_h_anim += (scrub_h_target - s_scrub_h_anim) * ImGui::GetIO().DeltaTime * 18.f;

            float mouse_t  = 0.f;
            bool  has_scrub_hov = false;
            if (scrub_hov || scrub_held) {
                float frac = (ImGui::GetIO().MousePos.x - scrub_x0) / scrub_w;
                frac = fmaxf(0.f, fminf(1.f, frac));
                mouse_t = frac * dur;
                mouse_t = roundf(mouse_t * 30.f) / 30.f;
                has_scrub_hov = true;
                if (scrub_held) seek_to(state, mouse_t);
            }

            float played_frac = fmaxf(0.f, fminf(1.f, state.playhead / dur));
            float play_sx = scrub_x0 + played_frac * scrub_w;
            float bh2 = s_scrub_h_anim * 0.5f;

            // Track
            dl->AddRectFilled({scrub_x0, scrub_cy - bh2}, {scrub_x1, scrub_cy + bh2},
                              IM_COL32(255,255,255,30), bh2);
            // Played
            dl->AddRectFilled({scrub_x0, scrub_cy - bh2}, {play_sx, scrub_cy + bh2},
                              IM_COL32(220,220,255,200), bh2);

            // Hover ghost
            if (has_scrub_hov) {
                float hsx = scrub_x0 + (mouse_t / dur) * scrub_w;
                dl->AddRectFilled({scrub_x0, scrub_cy - bh2}, {hsx, scrub_cy + bh2},
                                  IM_COL32(255,255,255,20), bh2);
                // Timecode bubble
                char htc[16]; snprintf(htc, sizeof(htc), "%s", fmt_time(mouse_t).c_str());
                float htc_w = ImGui::CalcTextSize(htc).x + 10.f;
                float htc_x = fmaxf(scrub_x0, fminf(hsx - htc_w*0.5f, scrub_x1 - htc_w));
                float htc_y = scrub_cy - bh2 - 22.f;
                dl->AddRectFilled({htc_x-2.f, htc_y-2.f}, {htc_x+htc_w+2.f, htc_y+16.f},
                                  IM_COL32(30,30,35,220), 4.f);
                dl->AddText({htc_x+5.f, htc_y+1.f}, IM_COL32(220,220,220,220), htc);
                // Hover dot
                dl->AddCircleFilled({hsx, scrub_cy}, s_scrub_h_anim + 1.f, IM_COL32(0,0,0,80));
                dl->AddCircleFilled({hsx, scrub_cy}, s_scrub_h_anim,       IM_COL32(255,255,255,160));
            }

            // Playhead knob
            float knob_r = (scrub_hov || scrub_held) ? s_scrub_h_anim + 2.f : s_scrub_h_anim;
            dl->AddCircleFilled({play_sx, scrub_cy}, knob_r + 1.5f, IM_COL32(0,0,0,120));
            dl->AddCircleFilled({play_sx, scrub_cy}, knob_r,        IM_COL32(255,255,255,255));

            // ── Thumbnail above pill on scrub hover ───────────────────────────
            if (has_scrub_hov) {
                float hsx = scrub_x0 + (mouse_t / dur) * scrub_w;
                int th_w = 0, th_h = 0;
                uintptr_t th_tex = video_get_thumbnail((double)mouse_t, &th_w, &th_h);
                if (th_tex && th_w > 0 && th_h > 0) {
                    float td_w = 120.f;
                    float td_h = td_w * (float)th_h / (float)th_w;
                    float tx = fmaxf(scrub_x0, fminf(hsx - td_w*0.5f, scrub_x1 - td_w));
                    float ty = pill_y0 - td_h - 8.f;
                    dl->AddRectFilled({tx-3.f,ty-3.f},{tx+td_w+3.f,ty+td_h+3.f},
                                      IM_COL32(20,20,20,220), 4.f);
                    dl->AddImage((ImTextureID)(uintptr_t)th_tex, {tx,ty}, {tx+td_w,ty+td_h});
                }
            }

            // ── Transport button row ──────────────────────────────────────────
            const float SB  = 26.f;
            const float PB  = 38.f;
            const float GAP = 8.f;
            float btns_total = SB * 4.f + PB + GAP * 4.f;
            float btn_row_y  = pill_y0 + PILL_PAD_Y + SCRUB_H + 10.f;
            float bx = pill_x0 + (PILL_W - btns_total) * 0.5f;
            float btn_cy = btn_row_y + BTN_ROW_H * 0.5f;

            // Glass circle button helper
            auto glass_btn = [&](const char* id, float sz) -> std::pair<bool, ImU32> {
                float cy2 = btn_row_y + (BTN_ROW_H - sz) * 0.5f;
                ImGui::SetCursorScreenPos({bx, cy2});
                ImGui::InvisibleButton(id, {sz, sz});
                bool h = ImGui::IsItemHovered();
                bool a = ImGui::IsItemActive();
                bool c = ImGui::IsItemClicked();
                float cx2 = bx + sz * 0.5f, cy3 = cy2 + sz * 0.5f;
                // Glass circle bg
                dl->AddCircleFilled({cx2, cy3}, sz * 0.5f,
                    IM_COL32(255,255,255, a ? 45 : h ? 28 : 12));
                dl->AddCircle({cx2, cy3}, sz * 0.5f - 0.5f,
                    IM_COL32(255,255,255, a ? 80 : h ? 50 : 22), 0, 1.f);
                ImU32 ic = IM_COL32(255,255,255, a ? 255 : h ? 230 : 180);
                bx += sz + GAP;
                return {c, ic};
            };

            // |< to start
            {
                auto [c, ic] = glass_btn("##t_start", SB);
                float cx2 = ImGui::GetItemRectMin().x + SB*0.5f;
                float r = SB * 0.22f;
                dl->AddRectFilled({cx2 - r*1.6f, btn_cy - r}, {cx2 - r*1.1f, btn_cy + r}, ic, 1.f);
                dl->AddTriangleFilled({cx2-r*0.9f, btn_cy-r}, {cx2-r*0.9f, btn_cy+r}, {cx2+r*0.9f, btn_cy}, ic);
                if (c) seek_to(state, 0.f);
            }

            // < frame back
            {
                auto [c, ic] = glass_btn("##t_prev", SB);
                float cx2 = ImGui::GetItemRectMin().x + SB*0.5f;
                float r = SB * 0.22f;
                dl->AddTriangleFilled({cx2+r, btn_cy-r}, {cx2+r, btn_cy+r}, {cx2-r, btn_cy}, ic);
                if (c) seek_to(state, fmaxf(0.f, state.playhead - f_dt_v));
            }

            // Play / Pause (larger glass circle)
            {
                float sz = PB;
                float cy2 = btn_row_y + (BTN_ROW_H - sz) * 0.5f;
                ImGui::SetCursorScreenPos({bx, cy2});
                ImGui::InvisibleButton("##t_play", {sz, sz});
                bool h = ImGui::IsItemHovered();
                bool a = ImGui::IsItemActive();
                bool c = ImGui::IsItemClicked();
                float cx2 = bx + sz*0.5f, cy3 = cy2 + sz*0.5f;
                // Glow ring
                dl->AddCircleFilled({cx2, cy3}, sz*0.5f + 2.f, IM_COL32(180,180,255, h||a ? 18 : 8));
                // Glass body
                dl->AddCircleFilled({cx2, cy3}, sz*0.5f,
                    IM_COL32(255,255,255, a ? 60 : h ? 42 : 25));
                dl->AddCircle({cx2, cy3}, sz*0.5f - 0.5f,
                    IM_COL32(255,255,255, a ? 100 : h ? 70 : 40), 0, 1.2f);
                // Top highlight arc — fake refraction
                dl->AddCircle({cx2, cy3 - 1.f}, sz*0.5f - 2.f,
                    IM_COL32(255,255,255, 18), 0, 1.f);

                ImU32 ic = IM_COL32(255,255,255, a ? 255 : h ? 235 : 200);
                float r = sz * 0.18f;
                if (busy) {
                    float t_spin = fmodf((float)ImGui::GetTime(), 1.2f) / 1.2f;
                    for (int i = 0; i < 3; ++i) {
                        float ang = (t_spin + i / 3.f) * 6.2832f;
                        dl->AddCircleFilled({cx2 + cosf(ang)*r, cy3 + sinf(ang)*r},
                                            2.2f, IM_COL32(255,255,255,200));
                    }
                } else if (state.playing) {
                    float bw = r*0.5f, bh3 = r*1.5f;
                    dl->AddRectFilled({cx2-bw*1.5f, cy3-bh3}, {cx2-bw*0.4f, cy3+bh3}, ic, 1.f);
                    dl->AddRectFilled({cx2+bw*0.4f, cy3-bh3}, {cx2+bw*1.5f, cy3+bh3}, ic, 1.f);
                } else {
                    dl->AddTriangleFilled({cx2-r*0.65f, cy3-r*1.05f},
                                          {cx2-r*0.65f, cy3+r*1.05f},
                                          {cx2+r*1.1f,  cy3}, ic);
                }
                if (c && !busy) toggle_play(state);
                bx += sz + GAP;
            }

            // > frame forward
            {
                auto [c, ic] = glass_btn("##t_next", SB);
                float cx2 = ImGui::GetItemRectMin().x + SB*0.5f;
                float r = SB * 0.22f;
                dl->AddTriangleFilled({cx2-r, btn_cy-r}, {cx2-r, btn_cy+r}, {cx2+r, btn_cy}, ic);
                if (c) seek_to(state, fminf(dur, state.playhead + f_dt_v));
            }

            // >| to end
            {
                auto [c, ic] = glass_btn("##t_end", SB);
                float cx2 = ImGui::GetItemRectMin().x + SB*0.5f;
                float r = SB * 0.22f;
                dl->AddTriangleFilled({cx2-r*0.9f, btn_cy-r}, {cx2-r*0.9f, btn_cy+r}, {cx2+r*0.9f, btn_cy}, ic);
                dl->AddRectFilled({cx2+r*1.1f, btn_cy-r}, {cx2+r*1.6f, btn_cy+r}, ic, 1.f);
                if (c) seek_to(state, dur);
            }

            // ── Timecode — centered row below buttons ─────────────────────────
            char tcbuf[32];
            snprintf(tcbuf, sizeof(tcbuf), "%s / %s",
                fmt_time(state.playhead).c_str(), fmt_time(dur).c_str());
            float tc_w = ImGui::CalcTextSize(tcbuf).x;
            float tc_y = btn_row_y + BTN_ROW_H + 2.f;
            float tc_x = pill_x0 + (PILL_W - tc_w) * 0.5f;
            dl->AddText({tc_x, tc_y}, IM_COL32(160,160,160,160), tcbuf);

            // Status text left of timecode when busy
            if (busy) {
                const char* st = audio_loading()       ? "loading…"
                               : proxy_is_generating() ? "building preview…"
                                                       : "extracting…";
                float st_w = ImGui::CalcTextSize(st).x;
                float st_x = pill_x0 + (PILL_W - st_w) * 0.5f;
                dl->AddText({st_x, tc_y}, IM_COL32(140,140,140,160), st);
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
            if (st != s_last_sel_track || sc != s_last_sel_clip) {
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
            auto tf = [&](PanelView target) -> ImGuiTabItemFlags {
                return (do_switch && s_panel_view == target) ? ImGuiTabItemFlags_SetSelected : 0;
            };

            if (ImGui::BeginTabBar("##panel_tabs")) {
                if (show_clip_tabs) {
                    if (ImGui::BeginTabItem("Clip", nullptr, tf(PanelView::Clip)))
                        { s_panel_view = PanelView::Clip; s_user_nav = false; ImGui::EndTabItem(); }
                    if (is_text_like && ImGui::BeginTabItem("Typography", nullptr, tf(PanelView::Typography)))
                        { s_panel_view = PanelView::Typography; s_user_nav = false; ImGui::EndTabItem(); }
                    if (ImGui::BeginTabItem("Animation", nullptr, tf(PanelView::Animation)))
                        { s_panel_view = PanelView::Animation; s_user_nav = true; ImGui::EndTabItem(); }
                }
                if (ImGui::BeginTabItem("History", nullptr, tf(PanelView::History)))
                    { s_panel_view = PanelView::History; s_user_nav = true; ImGui::EndTabItem(); }
                ImGui::EndTabBar();
            }

            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar();
        }

        // ── Panel content ─────────────────────────────────────────────────────
        ImGui::BeginChild("##panel_scroll", {0.f, 0.f});
        ImGui::SetCursorPosX(8.f);
        float pw = props_w - 16.f;

        switch (s_panel_view) {
            case PanelView::Clip:        panel_clip(state, pw);                  break;
            case PanelView::Animation:   panel_animation(state, pw);             break;
            case PanelView::Typography:  panel_typography(state, pw);            break;
            case PanelView::Project:     panel_project(state, pw);               break;
            case PanelView::History:     panel_history(state, pw);               break;
            case PanelView::LibBG:           panel_background(state, pw);            break;
            case PanelView::LibFX:           panel_fx_creative(state, pw);           break;
            case PanelView::LibAdj:          panel_adjustment_library(state, pw);    break;
            case PanelView::LibBFX:          panel_body_fx_library(state, pw);       break;
            case PanelView::LibAFX:          panel_fx_audio(state, pw);              break;
            case PanelView::LibVID:          panel_media_browser(state, pw, true);   break;
            case PanelView::LibIMG:          panel_media_browser(state, pw, false);  break;
            case PanelView::LibAUD:          panel_audio_browser(state, pw);         break;
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

    // ── Drag splitters ────────────────────────────────────────────────────────
    {
        static bool s_drag_vsplit = false, s_drag_hsplit = false;
        ImVec2 wpos = ImGui::GetWindowPos();
        ImVec2 mpos = ImGui::GetIO().MousePos;

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
        }
        if (ImGui::IsMouseReleased(0)) s_drag_vsplit = false;

        // Horizontal splitter between body and timeline
        float hborder_y = wpos.y + body_top + body_h + pipeline_h;
        bool near_h = fabsf(mpos.y - hborder_y) < 6.f &&
                      mpos.x > wpos.x &&
                      mpos.x < wpos.x + win_w;
        if (near_h || s_drag_hsplit) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            if (ImGui::IsMouseClicked(0)) s_drag_hsplit = true;
        }
        if (s_drag_hsplit) {
            float new_tl_h = wpos.y + body_top + avail_h - mpos.y;
            state.tl_h_frac = fmaxf(0.1f, fminf(0.7f, new_tl_h / avail_h));
        }
        if (ImGui::IsMouseReleased(0)) s_drag_hsplit = false;
    }

    // ── Pipeline strip ────────────────────────────────────────────────────────
    if (pipeline_h > 0.f) {
        ImGui::SetCursorPos({0.f, ImGui::GetCursorPosY()});
        draw_pipeline_strip(state, win_w);
    }
    draw_vision_download_strip(win_w);

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

        // Zoom controls in header
        {
            char zbuf[20]; snprintf(zbuf, sizeof(zbuf), "%.0f%%", state.tl_zoom / 80.f * 100.f);
            float zx = hdr_tl.x + hdr_w - 120.f;
            ImGui::SetCursorScreenPos({zx, hdr_tl.y+2.f});
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {4.f, 2.f});
            ImGui::PushStyleColor(ImGuiCol_Button,        Col::bg_soft);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Col::bg_soft_hov);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  Col::bg_soft_hov);
            if (ImGui::SmallButton("-##zout")) state.tl_zoom = fmaxf(state.tl_zoom*0.8f, 20.f);
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

    // ── Tutorial floating panel ───────────────────────────────────────────────
    if (state.show_tutorial && state.tutorial_step < 5) {
        // Auto-advance conditions
        if (state.tutorial_step == 0) {
            bool has_video = false;
            for (auto& tr : state.tracks)
                for (auto& cl : tr.clips)
                    if (cl.clip_type == ClipType::Video) { has_video = true; break; }
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
