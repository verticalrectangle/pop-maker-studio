#pragma once
#include <string>
#include <vector>
#include <functional>

// ── Track / clip data model ───────────────────────────────────────────────────

enum class TrackType { Subtitle, Audio, Video };

struct Clip {
    float       start   = 0.f;
    float       end     = 0.f;
    std::string text;           // subtitle tracks only
};

struct Track {
    TrackType            type;
    std::string          name;
    std::vector<Clip>    clips;
    bool                 visible = true;
    bool                 muted   = false;
    int                  sub_row = 0;   // vertical slot in preview (0=bottom)
};

// ── Pipeline state ────────────────────────────────────────────────────────────

enum class PipelineStage { Idle, Extract, Transcribe, Align, Done, Error };

struct PipelineStatus {
    PipelineStage stage = PipelineStage::Idle;
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
    bool    running      = false;
    float   progress     = 0.f;
    int     frame        = 0;
    int     total_frames = 0;
    float   eta_secs     = 0.f;
    std::string stage;
};

// ── App screen ────────────────────────────────────────────────────────────────

enum class Screen { Home, Upload, Editor, Styles, Export };

// ── Central app state ─────────────────────────────────────────────────────────

struct AppState {
    Screen current_screen = Screen::Home;

    // files
    std::string audio_path;
    std::string vocals_path;
    std::string words_json_path;

    // pipeline
    PipelineStatus pipeline;

    // timeline
    std::vector<Track> tracks;
    int   selected_track  = -1;
    int   selected_clip   = -1;

    // playback
    float playhead  = 0.f;
    bool  playing   = false;
    float duration  = 0.f;

    // timeline view
    float tl_scroll  = 0.f;   // horizontal scroll in pixels
    float tl_zoom    = 80.f;  // pixels per second

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

    void go(Screen s) { current_screen = s; }

    // Helpers for blender export / SRT — collect all subtitle clips
    // ordered by start time across all subtitle tracks
    std::vector<std::pair<int,int>> subtitle_clip_indices() const;
};

// ── App lifecycle ─────────────────────────────────────────────────────────────

void app_init(AppState& state);
void app_frame(AppState& state);
void app_shutdown(AppState& state);
