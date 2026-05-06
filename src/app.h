#pragma once
#include "presets.h"
#include <string>
#include <vector>
#include <chrono>
#include <unordered_map>
#include <set>
#include <array>
#include <algorithm>
#include <cmath>

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

static const int MAX_VIDEO_TRACKS = 8;

// ── Animation style ───────────────────────────────────────────────────────────

enum class AnimStyle {
    Fade, Glitch, Typewriter, Bounce, Scale,
    Slide, Stack, Block,
    None   // sentinel: inherit project default (state.style)
};

// ── Background removal status ─────────────────────────────────────────────────

enum class BgRemoveStatus { Idle, Processing, Ready, Error };

// ── Creative FX type ─────────────────────────────────────────────────────────

enum class FXType {
    Adjustment,   // brightness / contrast / blur / vignette (classic adjustment layer)
    Glitch,       // RGB channel split + row jitter
    ZoomPunch,    // beat-synced scale spike + shake
    LUT,          // 3D LUT color grade from .cube file
    LightLeak,    // procedural film-light flare synced to amplitude envelope
    VHS,          // chroma bleed + grain + tracking glitch
    Datamosh,     // temporal ghost buffer + multi-key chroma chaos
    ChromaKey,    // color-range keyer — compositing brick
};

// ── Transition type ───────────────────────────────────────────────────────────

enum class TransitionType { None, Dissolve, FadeBlack, DipWhite };

// ── Output format ─────────────────────────────────────────────────────────────

enum class OutputFormat { Vertical, Horizontal, Square };

// ── Track / clip data model ───────────────────────────────────────────────────

// Each clip carries its own type so any track can hold mixed content.
enum class ClipType { Text, Lyrics, Subtitle, Video, Audio, Effect };

struct WordEntry {
    std::string text;
    float start = 0.f;
    float end   = 0.f;
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
    int   sub_anchor_h = 1;    // horizontal anchor: 0=left 1=center 2=right
    float sub_color[4] = {1.f, 1.f, 1.f, 1.f};  // RGBA base / unspoken color
    bool  sub_color_override = false;
    float karaoke_highlight_color[4] = {1.f, 0.85f, 0.1f, 1.f};  // active word color

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

    // Creative FX (only active when fx_type != Adjustment)
    FXType      fx_type          = FXType::Adjustment;

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
    float       fx_datamosh_intensity  = 0.6f;  // ghost blend strength (0–1)
    float       fx_datamosh_decay      = 0.08f; // ghost self-feed rate (0–1, higher = more chaos)
    int         fx_datamosh_block_size = 16;    // MCU block size in pixels (8/16/32)
    float       fx_datamosh_bleedback  = 0.f;   // subject reasserts at tail (0=off, 1=full)

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

    // keyframe tracks — keyed by property name string
    // empty = use the matching static field above
    std::unordered_map<std::string, PropTrack> ktracks;

    // Evaluate named property at absolute timeline time `playhead`.
    // Falls back to the static field when no keyframes exist.
    float eval_prop(const std::string& name, float playhead) const;
};

struct Track {
    std::string       name;
    std::vector<Clip> clips;
    bool              visible = true;
    bool              muted   = false;
    bool              locked  = false;  // when true, blocks all clip edits on this track
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
    bool  datamosh_on            = false;
    float datamosh_intensity     = 0.6f;
    float datamosh_decay         = 0.08f;
    int   datamosh_block_size    = 16;
    float datamosh_clip_start    = -1.f;  // start time of the active datamosh clip
    float datamosh_bleedback     = 0.f;
    float datamosh_clip_duration = 0.f;

    bool  zoom_on        = false;
    float zoom_strength  = 0.f;
    float zoom_decay     = 0.15f;
    float zoom_shake     = 0.f;

    bool  leak_on        = false;
    float leak_intensity = 0.f;
    float leak_speed     = 1.f;

    bool  vhs_on         = false;
    float vhs_noise      = 0.f;
    float vhs_bleed      = 0.f;
    float vhs_tracking   = 0.f;
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
    bool        advanced_open = false;    // UI collapsible state
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
    Word,       // one clip per word (WhisperX default)
    Phrase,     // split on short pauses  > 0.3 s
    Line,       // split on breath gaps   > 0.8 s
    Segment,    // WhisperX sentence segments
    CustomN,    // user-defined N words per clip
    Karaoke,    // line-grouped clips with per-word highlight enabled
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
    float tl_scroll   = 0.f;
    float tl_zoom     = 80.f;
    float tl_v_scroll = 0.f;   // vertical scroll offset in the track area (pixels)

    // project settings
    int          fps         = 30;              // 24 / 30 / 60

    // style
    AnimStyle    style       = AnimStyle::Block;
    int          font_weight = 900;
    OutputFormat format      = OutputFormat::Vertical;

    // video background
    std::string video_path;
    bool        video_loaded   = false;
    bool        proxy_ready    = false;        // proxy ready for track 0 (backwards compat)
    bool        proxy_was_generating = false;  // tracks generation state changes

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
    std::string    out_wav;
    std::string    out_srt;

    // UI layout — user-dragged splitter positions (0 = auto)
    float panel_w   = 0.f;   // right panel width
    float tl_h_frac = 0.f;   // timeline height as fraction of body height (0 = auto)

    // audio extraction (ffmpeg demux, no ML)
    bool        extract_running      = false;
    bool        extract_done         = false;
    std::string extract_wav_path;
    int         extract_source_track = -1;  // track index of the video clip that was extracted

    // venv python
    std::string python_path = "/home/alexis/dev/song2subs/venv/bin/python";

    // model availability
    bool models_ready   = false;  // whisper + demucs weights detected on disk
    bool models_skipped = false;  // user chose "Skip for now" on setup screen

    // first-run multi-stage setup
    enum class SetupStage {
        Idle,
        PythonExtract,  // extracting embedded Python 3.11 tarball
        PipInstall,     // pip install whisperx demucs
        ModelDL,        // downloading model weights
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

    // tutorial
    bool show_tutorial  = false;
    int  tutorial_step  = 0;   // 0-4 = steps 1-5; >=5 = finished

    // noise reduction
    bool        noise_reduce_running  = false;
    float       noise_reduce_progress = 0.f;
    std::string noise_reduce_output;   // path to denoised WAV once done
    std::string noise_reduce_error;

    // subtitle grouping
    SubtitleMode subtitle_mode = SubtitleMode::Word;
    int          subtitle_n    = 3;   // words per clip for CustomN mode
    bool         pipeline_produces_subtitles = false;  // true = TranscribeOnly → Subtitle clips

    // right panel active tab: 0=Clip, 1=Animation, 2=Export, 3=History, 4=Lyrics, 5=FX Library
    int panel_tab = 0;

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
