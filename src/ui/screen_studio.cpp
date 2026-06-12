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
static bool s_user_nav = false; // user explicitly chose Animation/History tab
// Coupled Multi-FX brick shown in the selected content's FX tab this frame.
static int  s_host_fx_ti = -1;
static int  s_host_fx_ci = -1;

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
        history_push(state, "New project");  // baseline so the first edit is undoable
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
            Clip right = clip_split_at(clip, cut);
            track.clips.insert(track.clips.begin()+state.selected_clip+1, std::move(right));
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
    // If the terminal is focused, all drops belong to it so the studio
    // handler stands down. The terminal panel will inject the path at the
    // shell prompt later this frame.
    extern std::string g_dropped_file;
    bool term_claims_drop = state.terminal_open && terminal_is_focused();
    if (!g_dropped_file.empty() && !term_claims_drop) {
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

    // Per-slot video open/upgrade — handles four states:
    //   1. Already on the Proxy tier        → nothing to do (steady state)
    //   2. Proxy ready, slot is Still/Native → upgrade to Proxy (fastest scrub)
    //   3. No proxy yet, slot unopened       → open Native (libav direct decode)
    //                                          for instant preview, fall back to
    //                                          Still placeholder if libav can't
    //                                          open the file
    // Images never get an MJPEG proxy — skip them to avoid per-frame ffprobe spawns.
    for (int slot = 0; slot < MAX_VIDEO_TRACKS; ++slot) {
        const std::string& key = state.proxy_paths[slot];
        if (key.empty()) continue;
        if (video_source(slot) == PreviewSource::Proxy) continue;  // terminal state

        std::string src = source_from_key(key);
        if (is_image_path(src)) {
            // Images never get an MJPEG proxy — keep them out of the generic
            // native/proxy logic below (per-frame libav opens). Still is their
            // terminal state; repair both Closed slots (the add-time open
            // races the background still generator for brand-new files) and
            // slots stuck in Native (libav opens a PNG as a one-frame video,
            // then every decode past t=0 fails and the clip renders blank).
            if (video_source(slot) != PreviewSource::Still) {
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
                if (cl.text.empty() || is_image_path(cl.text)) continue;
                if (!proxy_is_ready(cl.text)) {
                    proxy_start(cl.text);
                    started = true;
                    break;
                }
            }
            if (started) break;
        }
    }

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

        // Pipeline completion is data-only: load the transcript / save SRTs / add
        // the vocals track for SeparateOnly runs. It NEVER mutates the timeline
        // with lyric clips on its own — callers (UI buttons, MCP) decide what
        // to do next via state.pipeline_on_done.
        PipelineMode m = state.last_pipeline_mode;
        if (m == PipelineMode::TranscribeOnly) {
            state.lyrics_edits.clear();
            load_words_cache(state);
            save_all_srts(state);
        } else if (m == PipelineMode::Both) {
            state.lyrics_edits.clear();
            load_words_cache(state);
            save_all_srts(state);
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
    recorder_tick(state);
    vrecorder_tick(state);

    // Push clip snapshots to audio system every frame.
    // The callback reads these to position audio correctly — no separate volume hack needed.
    {
        std::vector<AudioClipDesc> vdescs, adescs;
        for (auto& tr : state.tracks) {
            if (tr.muted) continue;
            int tr_idx = (int)(&tr - state.tracks.data());
            for (auto& cl : tr.clips) {
                // Record brick: the selected take plays like an audio clip
                // (in_point 0, speed 1). Muted while that brick is recording
                // so the previous pass doesn't bleed under the new one.
                if (cl.clip_type == ClipType::Record) {
                    int ci = (int)(&cl - tr.clips.data());
                    if (cl.muted || cl.rec_take_sel < 0 ||
                        cl.rec_take_sel >= (int)cl.rec_takes.size() ||
                        recorder_is_target(tr_idx, ci)) continue;
                    AudioClipDesc d;
                    d.tl_start = cl.start;    d.tl_end   = cl.end;
                    d.in_point = 0.f;         d.speed    = 1.f;
                    d.volume   = cl.volume;   d.pan      = cl.pan;
                    d.fade_in  = cl.fade_in;  d.fade_out = cl.fade_out;
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
                            if (own.any_active())
                                segs.push_back({0.f, cl.end - cl.start, own});
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
                history_push(state, "New project");  // baseline so the first edit is undoable
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
                        history_push(state, "Open project");  // baseline so the first edit is undoable
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
                Track& t = state.tracks[state.selected_track];
                Clip& c = t.clips[state.selected_clip];
                float cut = state.playhead;
                if (cut>c.start+0.02f && cut<c.end-0.02f) {
                    Clip r = clip_split_at(c, cut);
                    t.clips.insert(t.clips.begin()+state.selected_clip+1, std::move(r));
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
            if (state.agent_panel_open && !was_agent) state.terminal_open    = false;
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

            // ── Agent (in-app AI) ─────────────────────────────────────────────
            ImGui::Dummy({0.f, 16.f});
            ui_label("Agent");
            ImGui::Dummy({0.f, 4.f});
            {
                float lx = ImGui::GetStyle().WindowPadding.x + 8.f;
                static char s_key_buf[256] = {};
                AgentConfig acfg = agent_get_config();
                static char s_url_buf[256] = {};
                static char s_model_buf[128] = {};
                static bool s_synced = false;
                if (!s_synced) {
                    strncpy(s_url_buf,   acfg.base_url.c_str(), sizeof(s_url_buf)-1);
                    strncpy(s_model_buf, acfg.model.c_str(),    sizeof(s_model_buf)-1);
                    s_synced = true;
                }

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

                ImGui::Dummy({0.f, 6.f});
                ImGui::SetCursorPosX(lx);
                ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
                ImGui::TextUnformatted("Base URL"); ImGui::PopStyleColor();
                ImGui::SameLine(0.f, 8.f);
                ImGui::SetNextItemWidth(230.f);
                if (ImGui::InputText("##agent_url", s_url_buf, sizeof(s_url_buf))) {
                    acfg.base_url = s_url_buf; agent_set_config(acfg);
                }
                ImGui::SetCursorPosX(lx);
                ImGui::PushStyleColor(ImGuiCol_Text, Col::muted);
                ImGui::TextUnformatted("Model   "); ImGui::PopStyleColor();
                ImGui::SameLine(0.f, 8.f);
                ImGui::SetNextItemWidth(230.f);
                if (ImGui::InputText("##agent_model", s_model_buf, sizeof(s_model_buf))) {
                    acfg.model = s_model_buf; agent_set_config(acfg);
                }
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
            { "Bin",         PanelView::LibBin,  IM_COL32(220, 200, 120, 255), false },
            { "Backgrounds", PanelView::LibBG,   IM_COL32(180,  60, 160, 255), true  },
            { "Text",        PanelView::LibText, IM_COL32( 80, 140, 220, 255), false },
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
        // Fades in when the mouse enters the preview area and out when it
        // leaves — keeps the canvas clean while editing, surfaces on demand.
        // Stays visible while a button is being held mid-drag (s_active_latch)
        // and during long-running ops (`busy`) so progress is always readable.
        {
            ImDrawList* dl  = ImGui::GetWindowDrawList();
            float fps_v     = tl_fps(state);
            float f_dt_v    = fps_v > 0.f ? 1.f / fps_v : 1.f / 30.f;
            float dur       = fmaxf(state.duration, 0.01f);
            bool  busy      = audio_loading() || proxy_is_generating() || state.extract_running;

            // Hover hit-test against the preview rect (whole stage, not just
            // the pill — pill becomes interactable as soon as you enter).
            bool over_preview = ImGui::IsMouseHoveringRect(
                stage_p, {stage_p.x + sw, stage_p.y + sh});

            // Latched from prior frame: a transport button was active last
            // frame, so keep the pill visible through the drag even if the
            // mouse left the preview rect (released-outside cancels nicely).
            static bool s_active_latch = false;

            float target_op = (over_preview || s_active_latch || busy) ? 1.f : 0.f;
            static float s_op = 0.f;
            float rate = (target_op > s_op) ? 16.f : 6.f;  // fade in fast, out slow
            s_op += (target_op - s_op) * std::min(1.f, ImGui::GetIO().DeltaTime * rate);

            // Skip the entire block when invisible — no draws AND no
            // InvisibleButtons means hidden controls don't eat clicks.
            if (s_op < 0.02f) { s_active_latch = false; goto transport_overlay_end; }

            // Lambda can't capture the static — shadow it as a local.
            {
                float op = s_op;
                auto fa = [op](ImU32 c) -> ImU32 {
                    int a = (int)(((c >> 24) & 0xFF) * op + 0.5f);
                    if (a > 255) a = 255;
                    return (c & 0x00FFFFFFu) | ((uint32_t)a << 24);
                };
                bool any_active_this_frame = false;

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
                fa(IM_COL32(0,0,0,0)),   fa(IM_COL32(0,0,0,0)),
                fa(IM_COL32(0,0,0,160)), fa(IM_COL32(0,0,0,160)));

            // Glass pill body
            dl->AddRectFilled({pill_x0, pill_y0}, {pill_x1, pill_y1},
                              fa(IM_COL32(18, 18, 22, 210)), PILL_R);
            // Top-edge glass highlight
            dl->AddLine({pill_x0 + PILL_R, pill_y0 + 1.f},
                        {pill_x1 - PILL_R, pill_y0 + 1.f},
                        fa(IM_COL32(255,255,255,28)), 1.f);
            // Outer border
            dl->AddRect({pill_x0, pill_y0}, {pill_x1, pill_y1},
                        fa(IM_COL32(255,255,255,22)), PILL_R, 0, 1.f);

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
                              fa(IM_COL32(255,255,255,30)), bh2);
            // Played
            dl->AddRectFilled({scrub_x0, scrub_cy - bh2}, {play_sx, scrub_cy + bh2},
                              fa(IM_COL32(220,220,255,200)), bh2);

            if (scrub_held) any_active_this_frame = true;

            // Hover ghost
            if (has_scrub_hov) {
                float hsx = scrub_x0 + (mouse_t / dur) * scrub_w;
                dl->AddRectFilled({scrub_x0, scrub_cy - bh2}, {hsx, scrub_cy + bh2},
                                  fa(IM_COL32(255,255,255,20)), bh2);
                // Timecode bubble
                char htc[16]; snprintf(htc, sizeof(htc), "%s", fmt_time(mouse_t).c_str());
                float htc_w = ImGui::CalcTextSize(htc).x + 10.f;
                float htc_x = fmaxf(scrub_x0, fminf(hsx - htc_w*0.5f, scrub_x1 - htc_w));
                float htc_y = scrub_cy - bh2 - 22.f;
                dl->AddRectFilled({htc_x-2.f, htc_y-2.f}, {htc_x+htc_w+2.f, htc_y+16.f},
                                  fa(IM_COL32(30,30,35,220)), 4.f);
                dl->AddText({htc_x+5.f, htc_y+1.f}, fa(IM_COL32(220,220,220,220)), htc);
                // Hover dot
                dl->AddCircleFilled({hsx, scrub_cy}, s_scrub_h_anim + 1.f, fa(IM_COL32(0,0,0,80)));
                dl->AddCircleFilled({hsx, scrub_cy}, s_scrub_h_anim,       fa(IM_COL32(255,255,255,160)));
            }

            // Playhead knob
            float knob_r = (scrub_hov || scrub_held) ? s_scrub_h_anim + 2.f : s_scrub_h_anim;
            dl->AddCircleFilled({play_sx, scrub_cy}, knob_r + 1.5f, fa(IM_COL32(0,0,0,120)));
            dl->AddCircleFilled({play_sx, scrub_cy}, knob_r,        fa(IM_COL32(255,255,255,255)));

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
                                      fa(IM_COL32(20,20,20,220)), 4.f);
                    dl->AddImage((ImTextureID)(uintptr_t)th_tex, {tx,ty}, {tx+td_w,ty+td_h},
                                 {0,0}, {1,1}, fa(IM_COL32_WHITE));
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
                if (a) any_active_this_frame = true;
                float cx2 = bx + sz * 0.5f, cy3 = cy2 + sz * 0.5f;
                // Glass circle bg
                dl->AddCircleFilled({cx2, cy3}, sz * 0.5f,
                    fa(IM_COL32(255,255,255, a ? 45 : h ? 28 : 12)));
                dl->AddCircle({cx2, cy3}, sz * 0.5f - 0.5f,
                    fa(IM_COL32(255,255,255, a ? 80 : h ? 50 : 22)), 0, 1.f);
                ImU32 ic = fa(IM_COL32(255,255,255, a ? 255 : h ? 230 : 180));
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
                if (a) any_active_this_frame = true;
                float cx2 = bx + sz*0.5f, cy3 = cy2 + sz*0.5f;
                // Glow ring
                dl->AddCircleFilled({cx2, cy3}, sz*0.5f + 2.f, fa(IM_COL32(180,180,255, h||a ? 18 : 8)));
                // Glass body
                dl->AddCircleFilled({cx2, cy3}, sz*0.5f,
                    fa(IM_COL32(255,255,255, a ? 60 : h ? 42 : 25)));
                dl->AddCircle({cx2, cy3}, sz*0.5f - 0.5f,
                    fa(IM_COL32(255,255,255, a ? 100 : h ? 70 : 40)), 0, 1.2f);
                // Top highlight arc — fake refraction
                dl->AddCircle({cx2, cy3 - 1.f}, sz*0.5f - 2.f,
                    fa(IM_COL32(255,255,255, 18)), 0, 1.f);

                ImU32 ic = fa(IM_COL32(255,255,255, a ? 255 : h ? 235 : 200));
                float r = sz * 0.18f;
                if (busy) {
                    float t_spin = fmodf((float)ImGui::GetTime(), 1.2f) / 1.2f;
                    for (int i = 0; i < 3; ++i) {
                        float ang = (t_spin + i / 3.f) * 6.2832f;
                        dl->AddCircleFilled({cx2 + cosf(ang)*r, cy3 + sinf(ang)*r},
                                            2.2f, fa(IM_COL32(255,255,255,200)));
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
            dl->AddText({tc_x, tc_y}, fa(IM_COL32(160,160,160,160)), tcbuf);

            // Status text left of timecode when busy
            if (busy) {
                const char* st = audio_loading()       ? "loading…"
                               : proxy_is_generating() ? "building preview…"
                                                       : "extracting…";
                float st_w = ImGui::CalcTextSize(st).x;
                float st_x = pill_x0 + (PILL_W - st_w) * 0.5f;
                dl->AddText({st_x, tc_y}, fa(IM_COL32(140,140,140,160)), st);
            }

            s_active_latch = any_active_this_frame;
            }  // close inner scope opened with `float op = s_op;`
            transport_overlay_end:;
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

            // The FX tab appears only while the selected content has a
            // coupled Multi-FX brick on its track.
            int host_fx_ti = -1, host_fx_ci = -1;
            if (has_sel && state.selected_track >= 0 &&
                state.selected_track < (int)state.tracks.size()) {
                auto& cls = state.tracks[state.selected_track].clips;
                if (state.selected_clip >= 0 && state.selected_clip < (int)cls.size()) {
                    const Clip& sc = cls[(size_t)state.selected_clip];
                    bool hostable = clip_is_videolike_type(sc.clip_type) ||
                                    sc.clip_type == ClipType::Background;
                    if (hostable) {
                        for (int k = 0; k < (int)cls.size(); ++k) {
                            const Clip& oc = cls[(size_t)k];
                            if (oc.clip_type == ClipType::MultiFX && oc.fx_coupled &&
                                fx_coupled_host(state, state.selected_track, oc)
                                    == state.selected_clip) {
                                host_fx_ti = state.selected_track;
                                host_fx_ci = k;
                                break;
                            }
                        }
                    }
                }
            }
            if (s_panel_view == PanelView::HostFX && host_fx_ci < 0)
                s_panel_view = PanelView::Clip;   // brick decoupled/deleted

            if (ImGui::BeginTabBar("##panel_tabs")) {
                if (show_clip_tabs) {
                    if (ImGui::BeginTabItem("Clip", nullptr, tf(PanelView::Clip)))
                        { s_panel_view = PanelView::Clip; s_user_nav = false; ImGui::EndTabItem(); }
                    if (is_text_like && ImGui::BeginTabItem("Typography", nullptr, tf(PanelView::Typography)))
                        { s_panel_view = PanelView::Typography; s_user_nav = false; ImGui::EndTabItem(); }
                    if (host_fx_ci >= 0 && ImGui::BeginTabItem("FX", nullptr, tf(PanelView::HostFX)))
                        { s_panel_view = PanelView::HostFX; s_user_nav = false; ImGui::EndTabItem(); }
                }
                if (ImGui::BeginTabItem("History", nullptr, tf(PanelView::History)))
                    { s_panel_view = PanelView::History; s_user_nav = true; ImGui::EndTabItem(); }
                ImGui::EndTabBar();
            }
            s_host_fx_ti = host_fx_ti;
            s_host_fx_ci = host_fx_ci;

            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar();
        }

        // ── Panel content ─────────────────────────────────────────────────────
        ImGui::BeginChild("##panel_scroll", {0.f, 0.f});
        ImGui::SetCursorPosX(8.f);
        float pw = props_w - 16.f;

        switch (s_panel_view) {
            case PanelView::Clip:        panel_clip(state, pw);                  break;
            case PanelView::Typography:  panel_typography(state, pw);            break;
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

    // ── Drag splitters ────────────────────────────────────────────────────────
    {
        static bool s_drag_vsplit = false, s_drag_hsplit = false, s_drag_termsplit = false;
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
            state.tl_h_frac = fmaxf(0.1f, fminf(0.7f, new_tl_h / avail_h));
        }
        if (ImGui::IsMouseReleased(0)) s_drag_hsplit = false;

        // Horizontal splitter at the top edge of the terminal/agent strip
        if ((state.terminal_open || state.agent_panel_open) && term_h > 0.f) {
            float tborder_y = wpos.y + body_top + body_h + tl_h + pipeline_h;
            bool near_t = fabsf(mpos.y - tborder_y) < 6.f &&
                          mpos.x > wpos.x &&
                          mpos.x < wpos.x + win_w;
            if (near_t || s_drag_termsplit) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                if (ImGui::IsMouseClicked(0)) s_drag_termsplit = true;
            }
            if (s_drag_termsplit) {
                float new_term_h = wpos.y + body_top + avail_h - mpos.y;
                state.term_h_frac = fmaxf(0.f, fminf(0.6f, new_term_h / avail_h));
            }
            if (ImGui::IsMouseReleased(0)) s_drag_termsplit = false;
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
            // exceed this, since the zone deliberately can't scroll.
            float inp_h = 42.f;
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
            ImGui::PushStyleColor(ImGuiCol_Border,  Col::line);
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
