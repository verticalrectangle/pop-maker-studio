#pragma once
#include <string>
#include <vector>
#include <functional>

// ── Word / lyric data ─────────────────────────────────────────────────────────

struct Word {
    std::string text;
    float start = 0.f;
    float end   = 0.f;
};

struct LyricLine {
    std::vector<Word> words;
    // derived helpers
    float start_time() const { return words.empty() ? 0.f : words.front().start; }
    float end_time()   const { return words.empty() ? 0.f : words.back().end; }
    std::string full_text() const;
};

// ── Pipeline state ────────────────────────────────────────────────────────────

enum class PipelineStage { Idle, Extract, Transcribe, Align, Done, Error };

struct PipelineStatus {
    PipelineStage stage = PipelineStage::Idle;
    float         progress = 0.f;   // 0–1 within the current stage
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
    bool    running  = false;
    float   progress = 0.f;
    int     frame    = 0;
    int     total_frames = 0;
    float   eta_secs = 0.f;
    std::string stage;
};

// ── App screen ────────────────────────────────────────────────────────────────

enum class Screen { Home, Upload, Editor, Styles, Export };

// ── Central app state ─────────────────────────────────────────────────────────

struct AppState {
    Screen current_screen = Screen::Home;

    // file
    std::string audio_path;
    std::string vocals_path;
    std::string words_json_path;

    // pipeline
    PipelineStatus pipeline;

    // lyrics
    std::vector<LyricLine> lines;
    int   active_line = -1;
    int   active_word = -1;
    float playhead    = 0.f;
    bool  playing     = false;
    float duration    = 0.f;

    // style
    AnimStyle     style  = AnimStyle::Block;
    int           font_weight = 900;  // 400 / 700 / 900 maps to regular/bold/bold+scale
    OutputFormat  format = OutputFormat::Vertical;

    // video background
    std::string video_path;
    bool        video_loaded = false;

    // render
    RenderStatus render;
    bool         render_done = false;
    std::string  out_mp4;
    std::string  out_wav;
    std::string  out_srt;

    // venv python path — configurable, defaults to song2subs venv
    std::string python_path = "/home/alexis/dev/song2subs/venv/bin/python";

    void go(Screen s) { current_screen = s; }
};

// ── App lifecycle ─────────────────────────────────────────────────────────────

void app_init(AppState& state);
void app_frame(AppState& state);   // called each ImGui frame
void app_shutdown(AppState& state);
