#pragma once
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

// ── Track / clip data model ───────────────────────────────────────────────────

// Each clip carries its own type so any track can hold mixed content.
enum class ClipType { Text, Lyrics, Subtitle, Video, Audio };

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
    float volume         = 1.f;   // audio gain multiplier (0–2)
    float speed          = 1.f;   // playback speed (0.25–4)
    float opacity        = 1.f;   // video opacity (0–1)
    float transition_out = 0.f;   // crossfade out duration in seconds (video, applied on render)

    // subtitle-only overrides
    int   sub_pos   = 0;       // 0=bottom 1=center 2=top 3=custom Y
    float sub_pos_y = 0.85f;   // custom Y fraction from top (0=top, 1=bottom)
    float sub_color[4] = {1.f, 1.f, 1.f, 1.f};  // RGBA
    bool  sub_color_override = false;

    // per-clip transform (video clips; fractions of canvas size)
    float pos_x    = 0.5f;   // 0=left edge, 1=right edge (centre default)
    float pos_y    = 0.5f;   // 0=top edge,  1=bottom edge
    float scale_x  = 1.f;
    float scale_y  = 1.f;
    float rotation = 0.f;    // degrees, clockwise

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
    int               sub_row = 0;
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

enum class OutputFormat { Vertical, Horizontal, Square };

enum class AnimStyle {
    Fade, Glitch, Typewriter, Bounce, Scale,
    Slide, Stack, Block
};

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
};

// ── Central app state ─────────────────────────────────────────────────────────

struct AppState {
    // splash
    float splash_timer = 1.6f;  // counts down from launch; studio shows when <= 0

    // files
    std::string audio_path;
    std::string vocals_path;
    std::string words_json_path;    // <stem>_words.json
    std::string segments_json_path; // <stem>_segments.json
    std::vector<WordEntry> words_cache; // flat word list loaded from words_json_path

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
    float tl_scroll = 0.f;
    float tl_zoom   = 80.f;

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
    bool        extract_running  = false;
    bool        extract_done     = false;
    std::string extract_wav_path;

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

    // subtitle grouping
    SubtitleMode subtitle_mode = SubtitleMode::Word;
    int          subtitle_n    = 3;   // words per clip for CustomN mode
    bool         pipeline_produces_subtitles = false;  // true = TranscribeOnly → Subtitle clips

    // right panel active tab: 0=Clip, 1=Style, 2=Export, 3=History
    int panel_tab = 0;

    std::vector<std::pair<int,int>> subtitle_clip_indices() const;
};

// ── App lifecycle ─────────────────────────────────────────────────────────────

void app_init(AppState& state);
void app_frame(AppState& state);
void app_shutdown(AppState& state);
