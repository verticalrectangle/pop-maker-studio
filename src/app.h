#pragma once
#include "presets.h"
#include "audio_fx.h"
#include "body_fx.h"
#include <string>
#include <vector>
#include <deque>
#include <chrono>
#include <map>
#include <unordered_map>
#include <set>
#include <array>
#include <algorithm>
#include <cmath>
#include <functional>

struct AppState;

enum class PipelineMode {
    Both,            // Demucs + transcription
    TranscribeOnly,  // transcription on original file (no Demucs)
    SeparateOnly,    // Demucs only, no subtitles
};

// ── Keyframing ────────────────────────────────────────────────────────────────

enum class InterpType { Linear, EaseIn, EaseOut, EaseBoth, Hold };

struct Keyframe {
    float      time   = 0.f;   // seconds relative to clip.start
    float      value  = 0.f;
    InterpType interp = InterpType::EaseBoth;
};

struct PropTrack {
    std::vector<Keyframe> keys;  // always sorted by time

    bool  empty()                                                     const { return keys.empty(); }
    float eval(float t)                                               const;
    void  set(float t, float v, InterpType it = InterpType::EaseBoth);
    void  remove_at(float t, float tol = 0.05f);
    int   find_nearest(float t, float tol = 0.1f)                    const;
};

// ── Constants ─────────────────────────────────────────────────────────────────

static const int MAX_VIDEO_TRACKS = 32;

// ── Animation style ───────────────────────────────────────────────────────────

enum class AnimStyle {
    Fade, Glitch, Typewriter, Bounce, Scale,
    Slide, Stack, Block,
    None   // sentinel: inherit project default (state.style)
};

// ── Background removal status ─────────────────────────────────────────────────

enum class BgRemoveStatus { Idle, WaitingForProxy, Processing, Ready, Error };
enum class VcStatus       { Idle, Processing, Ready, Error };

// ── Creative FX type ─────────────────────────────────────────────────────────

enum class FXType {
    Grade,        // colour grade: brightness / contrast / saturation / hue
    Blur,         // gaussian blur
    Vignette,     // radial vignette
    Glitch,       // RGB channel split + row jitter
    ZoomPunch,    // beat-synced scale spike + shake
    LUT,          // 3D LUT color grade from .cube file
    LightLeak,    // procedural film-light flare synced to amplitude envelope
    VHS,          // chroma bleed + grain + tracking glitch
    Datamosh,     // temporal ghost buffer + multi-key chroma chaos
    ChromaKey,    // color-range keyer — compositing brick
    // ── Audio FX bricks ─────────────────────────────────────────────────────
    AudioAutotune,      // YIN pitch detection + grain shift + scale quantize
    AudioPitch,         // grain pitch shift, fixed semitone offset
    AudioFormant,       // formant shift (voice character colouring)
    AudioDelay,         // stereo delay with feedback
    AudioReverb,        // Freeverb-style room reverb
    AudioVoiceConvert,  // RVC-based voice conversion (requires .pth model)
#include "generated/fx_enum_entries.h"
};

// ── Transition type ───────────────────────────────────────────────────────────

enum class TransitionType { None, Dissolve, FadeBlack, DipWhite };

// ── Output format ─────────────────────────────────────────────────────────────

enum class OutputFormat { Vertical, Horizontal, Square };

// ── Track / clip data model ───────────────────────────────────────────────────

// Each clip carries its own type so any track can hold mixed content.
enum class ClipType { Text, Lyrics, Subtitle, Video, Audio, Effect, Background, BodyFX, MultiFX };

struct WordEntry {
    std::string text;
    float start = 0.f;
    float end   = 0.f;
};

struct TextStyle {
    bool  shadow_enabled = true;
    float shadow_ox      = 2.f, shadow_oy = 2.f;
    float shadow_col[4]  = {0.f, 0.f, 0.f, 0.7f};

    bool  stroke_enabled = false;
    float stroke_w       = 2.f;
    float stroke_col[4]  = {0.f, 0.f, 0.f, 1.f};

    bool  glow_enabled   = false;
    float glow_r         = 6.f;
    float glow_col[4]    = {1.f, 1.f, 1.f, 0.4f};

    bool  bg_enabled     = false;
    float bg_col[4]      = {1.f, 1.f, 1.f, 1.f};
    float bg_pad_x       = 8.f, bg_pad_y = 4.f, bg_corner = 0.f;
};

struct Clip {
    ClipType    clip_type = ClipType::Text;  // Text/Video/Audio — independent of track type
    float       start = 0.f;
    float       end   = 0.f;
    std::string text;
    std::string source_id;  // file path or audio path that produced this clip; groups related clips

    // per-clip overrides (Tier 1)
    bool  muted          = false; // silences audio output for this clip; shown as icon on brick
    float volume         = 1.f;   // audio gain multiplier (0–2)
    float speed          = 1.f;   // playback speed (0.25–4)
    float opacity        = 1.f;   // video opacity (0–1)
    float          transition_pre  = 0.f;               // seconds the transition extends into this clip (before cut)
    float          transition_post = 0.f;               // seconds the transition extends into the next clip (after cut)
    TransitionType transition_type = TransitionType::None; // transition to next clip on same track

    // fade in/out — opacity ramp applied at render & preview when no manual opacity KFs exist
    float fade_in  = 0.f;   // seconds from clip start to full opacity
    float fade_out = 0.f;   // seconds before clip end to begin fade to zero

    // source trim (Video/Audio) — which portion of the source file plays
    float in_point  = 0.f;    // seconds into source file to begin playback
    float out_point = -1.f;   // seconds into source file to end (-1 = until end of source)

    // stereo pan: -1=full left, 0=center, +1=full right (Video embedded audio + Audio clips)
    float pan = 0.f;

    // blend mode for video compositing: 0=Normal 1=Multiply 2=Screen 3=Overlay
    int blend_mode = 0;

    // subtitle-only overrides
    bool  karaoke   = false;   // per-word highlight enabled; available on Phrase/Line/Segment/CustomN
    int   sub_pos   = 0;       // 0=bottom 1=center 2=top 3=custom Y
    float sub_pos_y = 0.85f;   // custom Y fraction from top (0=top, 1=bottom)
    float sub_pos_x = 0.5f;    // horizontal center fraction (0=left, 1=right)
    float sub_wrap_w = 0.85f;  // text column width as fraction of canvas width (word wrap)
    float font_size  = 0.f;    // subtitle font size as fraction of canvas height (0 = project default)
    int   sub_anchor_h = 1;    // horizontal anchor: 0=left 1=center 2=right
    float sub_color[4] = {1.f, 1.f, 1.f, 1.f};  // RGBA base / unspoken color
    bool  sub_color_override = false;
    float karaoke_highlight_color[4] = {1.f, 0.85f, 0.1f, 1.f};  // active word color

    TextStyle ts;

    // callout overlay fields (text clips only)
    int   callout_style = 0;    // 0=none 1=box 2=pill 3=speech_bubble
    bool  callout_arrow = false;
    float arrow_tx      = 0.5f; // canvas 0–1, arrow target point
    float arrow_ty      = 0.5f;

    // per-clip word list for deep lyrics editing (populated by apply_subtitle_mode)
    std::vector<WordEntry> words;

    // per-clip transform (video clips; fractions of canvas size)
    float pos_x    = 0.5f;   // 0=left edge, 1=right edge (centre default)
    float pos_y    = 0.5f;   // 0=top edge,  1=bottom edge
    float scale_x  = 1.f;
    float scale_y  = 1.f;
    float rotation = 0.f;    // degrees, clockwise

    // per-clip animation style (None = inherit project default)
    AnimStyle   clip_style = AnimStyle::None;

    // per-clip color grade (Video/Background; accumulated in collect_glass_effects)
    float grade_brightness = 0.f;
    float grade_contrast   = 1.f;
    float grade_saturation = 1.f;
    float grade_hue        = 0.f;

    // Effect clip properties (ClipType::Effect only)
    bool  fx_color_on    = false;
    float fx_brightness  = 0.f;
    float fx_contrast    = 1.f;
    float fx_saturation  = 1.f;
    float fx_hue         = 0.f;
    bool  fx_blur_on     = false;
    float fx_blur        = 0.f;
    bool  fx_vignette_on = false;
    float fx_vignette    = 0.f;
    bool  fx_text_on     = false;
    float fx_opacity_mul = 1.f;
    float fx_scale_mul   = 1.f;

    FXType      fx_type          = FXType::Grade;

    // ChromaKey brick
    float       fx_chroma_key_r         = 0.f;
    float       fx_chroma_key_g         = 1.f;
    float       fx_chroma_key_b         = 0.f;
    float       fx_chroma_key_threshold = 0.30f;
    float       fx_chroma_key_softness  = 0.15f;

    // Glitch
    float       fx_glitch_chroma     = 8.f;   // RGB channel spread in pixels
    float       fx_glitch_jitter     = 0.3f;  // row-shift intensity (0–1)
    float       fx_glitch_corruption       = 0.2f;  // JPEG block corruption intensity (0–1)
    float       fx_glitch_corruption_bleed = 0.f;   // 0=noise only, 1=transparent holes

    // Datamosh
    float       fx_datamosh_intensity  = 0.6f;
    float       fx_datamosh_spread     = 0.3f;

    // ZoomPunch
    float       fx_zoom_strength = 0.08f; // peak scale-up fraction (0–0.5)
    float       fx_zoom_decay    = 0.15f; // seconds to settle back (0.05–0.5)
    float       fx_zoom_shake    = 0.3f;  // random positional shake (0–1)

    // LUT
    std::string fx_lut_path;              // absolute path to .cube file

    // LightLeak
    float       fx_leak_intensity = 0.6f; // blend opacity (0–1)
    float       fx_leak_speed     = 1.f;  // flare animation speed (0–4)

    // VHS
    float       fx_vhs_noise      = 0.3f; // static grain density (0–1)
    float       fx_vhs_bleed      = 8.f;  // chroma bleed pixels (0–20)
    float       fx_vhs_tracking   = 0.2f; // tracking glitch warp (0–1)

    // Remove Background
    bool          bg_remove_on       = false;
    float         bg_remove_softness = 0.1f;   // edge feather (0=hard)
    std::string   bg_remove_mask_dir;           // proxy-res PNG masks (empty = not processed)
    BgRemoveStatus bg_remove_status  = BgRemoveStatus::Idle;
    float         bg_remove_progress = 0.f;
    std::string   bg_remove_error;
    // Bounding box — crop mask to a rectangular area (0=left/top, 1=right/bottom)
    bool          bg_remove_box_on   = false;
    float         bg_remove_box_l    = 0.f;
    float         bg_remove_box_r    = 1.f;
    float         bg_remove_box_t    = 0.f;
    float         bg_remove_box_b    = 1.f;

    // Voice conversion job state (explicit, like rembg)
    VcStatus    vc_status   = VcStatus::Idle;
    float       vc_progress = 0.f;
    std::string vc_out_path;    // converted WAV path when Ready
    std::string vc_model_used;  // model .pth that produced vc_out_path
    std::string vc_error;

    // Generated effect clip fields
#include "generated/fx_clip_fields.h"

    // Background clip fields (ClipType::Background; text = preset id)
    float bg_speed     = 1.f;
    float bg_intensity = 0.85f;
    float bg_c1[4]    = {0.4f, 0.0f, 0.8f, 1.f};
    float bg_c2[4]    = {0.0f, 0.8f, 1.0f, 1.f};
    float bg_c3[4]    = {1.0f, 0.1f, 0.5f, 1.f};

    // Runtime FX (hot-reload custom effect from effects directory)
    std::string          runtime_fx_id;              // id of active runtime effect ("" = none)
    std::vector<float>   runtime_fx_params;           // one float per param, in registry order
    float                runtime_fx_amount = 1.f;

    // BodyFX brick (ClipType::BodyFX) — always glass; masks from sibling video clip's bg_remove
    BodyFXType     body_fx_type          = BodyFXType::RemoveBackground;
    float          body_fx_params[4]     = {0.5f, 0.5f, 0.5f, 0.5f};
    float          body_fx_amount        = 1.f;

    // Beat sync fields (Audio/Video clips: analyzed beats; FX clips: source reference)
    std::vector<float> beats;           // beat timestamps (Audio/Video clips only)
    float              beat_bpm   = 0.f;
    std::string        beats_json_path; // path to beats.json cache
    bool               beats_analyzing = false; // background analysis in progress

    int   beat_src_track = -1;   // FX clip: which track holds the audio source clip
    int   beat_src_clip  = -1;   // FX clip: which clip index on that track
    float beat_decay     = 0.15f; // FX clip: exp decay time in seconds

    // Audio FX chain (Audio/Video clips with embedded audio)
    AudioFX audio_fx;

    // keyframe tracks — keyed by property name string
    // empty = use the matching static field above
    std::unordered_map<std::string, PropTrack> ktracks;

    // Evaluate named property at absolute timeline time `playhead`.
    // Falls back to the static field when no keyframes exist.
    float eval_prop(const std::string& name, float playhead) const;

    // Multi-FX chain (ClipType::MultiFX only).
    // rel_start/rel_end: seconds from this clip's .start (rel_end==0 → full duration).
    float              rel_start = 0.f;
    float              rel_end   = 0.f;
    std::vector<Clip>  fx_chain;          // ordered sub-effects for MultiFX bricks
    int                fx_chain_selected = -1;
};

struct Track {
    std::string       name;
    std::vector<Clip> clips;
    bool              visible = true;
    bool              muted   = false;
    bool              locked  = false;  // when true, blocks all clip edits on this track
    bool              managed = false;  // owned by typography system — preset rewrites clips in-place
    int               sub_row = 0;
};

// ── Creative FX accumulator ───────────────────────────────────────────────────

struct CreativeFXAccum {
    // ChromaKey
    bool  chroma_key_on        = false;
    float chroma_key_r         = 0.f;
    float chroma_key_g         = 1.f;
    float chroma_key_b         = 0.f;
    float chroma_key_threshold = 0.30f;
    float chroma_key_softness  = 0.15f;

    bool  glitch_on         = false;
    float glitch_chroma     = 0.f;
    float glitch_jitter     = 0.f;
    float glitch_corruption       = 0.f;
    float glitch_corruption_bleed = 0.f;

    // Datamosh
    bool  datamosh_on        = false;
    float datamosh_intensity = 0.6f;
    float datamosh_spread    = 0.3f;

    bool  zoom_on        = false;
    float zoom_strength  = 0.f;
    float zoom_decay     = 0.15f;
    float zoom_shake     = 0.f;
    int   zoom_src_track = -1;
    int   zoom_src_clip  = -1;

    bool  leak_on        = false;
    float leak_intensity = 0.f;
    float leak_speed     = 1.f;

    bool  vhs_on         = false;
    float vhs_noise      = 0.f;
    float vhs_bleed      = 0.f;
    float vhs_tracking   = 0.f;

    // Generated creative FX fields
#include "generated/fx_accum_fields.h"

    bool any_gen_fx = false;  // any generated (codegen) creative FX is active
    bool any_cfx    = false;  // any hardcoded creative FX is active (chroma key, glitch, VHS, etc.)
};

// ── Effect accumulator ────────────────────────────────────────────────────────

struct EffectAccum {
    float brightness  = 0.f;
    float contrast    = 1.f;
    float saturation  = 1.f;
    float hue         = 0.f;
    float blur        = 0.f;
    float vignette    = 0.f;
    float opacity_mul = 1.f;
    float scale_mul   = 1.f;
    bool  any_color   = false;
    bool  any_blur    = false;
    bool  any_vignette= false;
    bool  any_text    = false;
};

// ── Pipeline state ────────────────────────────────────────────────────────────

enum class PipelineStage { Idle, Extract, Transcribe, Align, Done, Error };

struct PipelineStatus {
    PipelineStage stage    = PipelineStage::Idle;
    float         progress = 0.f;
    std::string   message;
    std::string   error;
    std::string   raw_line;  // last raw line from subprocess (debug)
};

// ── Render settings ───────────────────────────────────────────────────────────

struct RenderSettings {
    int         crf           = 23;       // 0=lossless … 51=worst; 23 is default
    int         audio_bitrate = 192;      // kbps: 128 / 192 / 320
    std::string preset        = "medium"; // ultrafast/fast/medium/slow/veryslow
    bool        high_profile  = false;    // false=Main, true=High
    bool        use_vaapi     = true;     // use VAAPI HW encoder when available (AMD/Intel on Linux)
    bool        advanced_open = false;    // UI collapsible state
    bool        gif_export    = false;    // output animated GIF instead of MP4
    int         gif_fps       = 12;       // GIF playback framerate (8/12/15/24)
    int         gif_scale     = 50;       // output scale percent (25/50/75/100)
};

// ── Render state ──────────────────────────────────────────────────────────────

struct RenderStatus {
    bool        running      = false;
    float       progress     = 0.f;
    int         frame        = 0;
    int         total_frames = 0;
    float       eta_secs     = 0.f;
    std::string stage;
};

// ── Subtitle grouping mode ────────────────────────────────────────────────────

enum class SubtitleMode {
    Word,       // one clip per word
    Phrase,     // split on short pauses  > 0.3 s
    Line,       // split on breath gaps   > 0.8 s
    Segment,    // sentence segments
    CustomN,    // user-defined N words per clip
    Karaoke,    // line-grouped clips with per-word highlight enabled
};

// ── Chapter markers ───────────────────────────────────────────────────────────

struct Marker {
    float       time  = 0.f;
    std::string label;
    uint32_t    color = 0xFF4A90E2u;  // ABGR, default cornflower blue
};

// ── Central app state ─────────────────────────────────────────────────────────

struct AppState {
    // splash
    float splash_timer = 1.6f;  // counts down from launch; studio shows when <= 0

    // files
    std::string project_path;   // path of the .pms file last saved/loaded (empty = unsaved)
    std::string audio_path;
    std::string vocals_path;
    std::string words_json_path;    // <stem>_words.json
    std::string segments_json_path; // <stem>_segments.json
    std::vector<WordEntry> words_cache; // flat word list loaded from words_json_path
    std::map<int, std::string> lyrics_edits; // key: int(word.start * fps), value: edited word text

    // chapter markers (sorted by time)
    std::vector<Marker> markers;

    // beat sync
    std::vector<float> beats;           // beat timestamps in seconds
    float              beat_bpm  = 0.f;
    std::string        beats_json_path;
    bool               beats_running = false;

    // amplitude envelope
    std::vector<float> amplitude_envelope; // normalized RMS, one value per envelope frame
    float              envelope_fps  = 0.f;
    std::string        envelope_json_path;
    bool               envelope_running = false;

    // pipeline
    PipelineStatus pipeline;

    // timeline
    std::vector<Track> tracks;
    int   selected_track = -1;
    int   selected_clip  = -1;

    // playback
    float playhead = 0.f;
    bool  playing  = false;
    float duration = 0.f;

    // wall-clock playback sync
    std::chrono::steady_clock::time_point play_start_wall;
    float play_start_pos = 0.f;

    // timeline view
    float tl_scroll       = 0.f;
    float tl_zoom         = 80.f;
    float tl_v_scroll     = 0.f;   // vertical scroll offset in the track area (pixels)
    float tl_clip_area_w      = 0.f;   // visible pixel width (written each frame by draw_timeline)
    float tl_zoom_to_fit_end  = 0.f;   // when >0, draw_timeline zooms out to fit this time + padding then clears it
    float tl_zoom_min         = 1.f;   // fit-to-width floor, recomputed each frame by draw_timeline

    // project settings
    int          fps         = 30;              // 24 / 30 / 60

    // style
    AnimStyle    style       = AnimStyle::None;
    int          font_weight = 900;
    OutputFormat format      = OutputFormat::Vertical;

    // video background
    std::string video_path;
    bool        video_loaded   = false;
    bool        proxy_ready    = false;        // proxy ready for track 0 (backwards compat)
    bool        proxy_was_generating = false;  // tracks generation state changes
    bool        proxy_scan_needed = false;     // set by IPC after adding video clips

    // Proxy slot table: proxy_paths[slot] = source file path (empty = free).
    // Keyed by file path so two clips sharing a source share one proxy.
    std::string proxy_paths[MAX_VIDEO_TRACKS];

    // keyframe selection
    int         kf_sel_track = -1;
    int         kf_sel_clip  = -1;
    std::string kf_sel_prop;
    int         kf_sel_idx   = -1;

    // multi-clip selection — set of (track_idx, clip_idx)
    std::set<std::pair<int,int>> clip_selection;

    // expanded clips in timeline (track_idx, clip_idx)
    std::set<std::pair<int,int>> expanded_clips;

    // render
    RenderStatus   render;
    RenderSettings render_settings;
    bool           render_done = false;
    std::string    out_mp4;
    std::string    out_gif;
    std::string    out_wav;
    std::string    out_srt;

    // UI layout — user-dragged splitter positions (0 = auto)
    float panel_w    = 0.f;   // right panel width
    float tl_h_frac  = 0.f;   // timeline height as fraction of avail_h (0 = auto)
    float term_h_frac = 0.f;  // terminal height as fraction of avail_h (0 = auto)

    // audio extraction (ffmpeg demux, no ML)
    bool        extract_running      = false;
    bool        extract_done         = false;
    std::string extract_wav_path;
    int         extract_source_track = -1;  // track index of the video clip that was extracted

    // model availability
    bool models_ready   = false;  // whisper + separate weights detected on disk
    bool models_skipped = false;  // user chose "Skip for now" on setup screen

    // first-run multi-stage setup
    enum class SetupStage {
        Idle,
        ModelDL,  // downloading model weights
        Done,
        Error
    };
    SetupStage  setup_stage    = SetupStage::Idle;
    bool        setup_running  = false;
    float       setup_progress = 0.f;   // 0–1 within current stage
    std::string setup_message;
    std::string setup_error_msg;

    // re-download modal (from Help menu — skips Python/pip, just re-runs prefetch)
    bool        model_dl_running  = false;
    bool        model_dl_done     = false;
    bool        model_dl_error    = false;
    float       model_dl_progress = 0.f;
    std::string model_dl_stage;
    std::string model_dl_message;
    std::string model_dl_error_msg;
    bool        show_model_dl_modal = false;
    bool        show_settings_modal = false;
    bool        show_export_modal   = false;

    // tutorial
    bool show_tutorial  = false;
    int  tutorial_step  = 0;   // 0-4 = steps 1-5; >=5 = finished

    // scroll-to-clip request (set by preview click, consumed by draw_timeline)
    bool request_scroll_to_clip = false;

    // frame snapshot
    bool        snapshot_running  = false;
    std::string snapshot_msg;           // shown in preview corner after save
    double      snapshot_msg_t    = 0.0; // ImGui::GetTime() when message was set
    bool        snapshot_msg_new  = false; // render thread sets; UI stamps time

    // IPC-requested snapshot (ipc_server sets request; GL thread fulfills and sets done)
    bool        snapshot_request    = false;
    bool        snapshot_done       = false;
    std::string snapshot_done_path;
    std::string snapshot_done_err;

    // IPC-requested export (ipc_server sets; GL thread calls render_start_gl)
    bool        export_request  = false;
    std::string export_out_path;

    // noise reduction
    bool        noise_reduce_running  = false;
    float       noise_reduce_progress = 0.f;
    std::string noise_reduce_output;   // path to denoised WAV once done
    std::string noise_reduce_error;

    // subtitle grouping (legacy, kept for project compat)
    SubtitleMode subtitle_mode = SubtitleMode::Word;
    int          subtitle_n    = 3;

    // Pipeline completion state. kick_pipeline() records the mode it was launched
    // with so the async completion handler knows what kind of work just finished
    // (transcript vs separation). pipeline_on_done is an optional callback the
    // caller sets *before* kicking the pipeline — UI buttons use it to chain
    // apply_subtitle_mode or generate_typography after completion. MCP
    // trigger_pipeline leaves it null so the timeline is never mutated.
    PipelineMode                       last_pipeline_mode = PipelineMode::Both;
    std::function<void(AppState&)>     pipeline_on_done;

    // typography
    std::string  typo_preset_id  = "flash";     // active preset id
    // tune overrides (-1 / 0 = use preset default)
    float        typo_font_size  = 0.f;       // 0 = use preset
    float        typo_color[4]   = {0.f,0.f,0.f,0.f}; // all-zero = use preset
    bool         typo_all_caps_override = false;
    bool         typo_all_caps   = false;
    SubtitleMode typo_grouping   = SubtitleMode::Word; // mirrors preset until overridden
    int          typo_custom_n   = 3;

    // right panel active tab: 0=Clip, 1=Animation, 2=Export, 3=History, 4=Lyrics, 5=FX Library
    int panel_tab = 0;

    // MCP agent status — set by begin_batch / end_batch IPC calls
    bool        agent_active = false;
    std::string agent_msg;   // batch label shown in loading panel

    // Rolling log of recent MCP tool calls (newest first, max 6)
    struct AgentOp {
        std::string method;   // raw method name
        std::string detail;   // file basename, query snippet, etc.
    };
    std::deque<AgentOp> agent_log;

    // Terminal panel
    bool  terminal_open   = false;

    // user-created effect presets (persisted to ~/.config/pop-maker-studio/presets.json)
    std::vector<EffectPreset> user_presets;

    std::vector<std::pair<int,int>> subtitle_clip_indices() const;
};

// ── App lifecycle ─────────────────────────────────────────────────────────────

void app_init(AppState& state);
void app_frame(AppState& state);
void app_shutdown(AppState& state);

// Accumulate all Effect clips on tracks above below_track_idx that are active at time t.
EffectAccum      collect_effects     (const AppState& state, float t, int below_track_idx);
CreativeFXAccum  collect_creative_fx (const AppState& state, float t, int below_track_idx);
// Glass FX/adjustments: bricks directly above a video/audio clip (clip-specific, pre-composite)
EffectAccum      collect_glass_effects(const AppState& state, float t, int video_track_idx);
CreativeFXAccum  collect_glass_fx    (const AppState& state, float t, int video_track_idx);
// Single-track variants: accumulate only from the given track (global/non-glass only).
// Used by the scene compositor to apply per-track global FX in z-order.
EffectAccum      collect_effects_for_track     (const AppState& state, float t, int track_idx);
CreativeFXAccum  collect_creative_fx_for_track (const AppState& state, float t, int track_idx);
// Visual check: does this FX clip sit directly above any video/audio clip (time overlap)?
bool             fx_clip_is_glass    (const AppState& state, int fx_ti, const Clip& fx_cl);

// Beat sync: returns exp-decay pulse [0..1] from most recent beat before t in the referenced clip.
// src_track/src_clip = -1 → always returns 0.
float            beat_pulse_at       (const AppState& state, int src_track, int src_clip,
                                      float t, float decay);
