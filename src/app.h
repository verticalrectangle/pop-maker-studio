#pragma once
#include <string>
#include <vector>

// ── Track / clip data model ───────────────────────────────────────────────────

enum class TrackType { Subtitle, Audio, Video };

struct Clip {
    float       start = 0.f;
    float       end   = 0.f;
    std::string text;

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
};

struct Track {
    TrackType         type;
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

    // timeline view
    float tl_scroll = 0.f;
    float tl_zoom   = 80.f;

    // style
    AnimStyle    style       = AnimStyle::Block;
    int          font_weight = 900;
    OutputFormat format      = OutputFormat::Vertical;

    // video background
    std::string video_path;
    bool        video_loaded = false;

    // render
    RenderStatus render;
    bool         render_done = false;
    std::string  out_mp4;
    std::string  out_wav;
    std::string  out_srt;

    // venv python
    std::string python_path = "/home/alexis/dev/song2subs/venv/bin/python";

    // subtitle grouping
    SubtitleMode subtitle_mode = SubtitleMode::Word;
    int          subtitle_n    = 3;   // words per clip for CustomN mode

    // right panel active tab: 0=Clip, 1=Style, 2=Export
    int panel_tab = 0;

    std::vector<std::pair<int,int>> subtitle_clip_indices() const;
};

// ── App lifecycle ─────────────────────────────────────────────────────────────

void app_init(AppState& state);
void app_frame(AppState& state);
void app_shutdown(AppState& state);
