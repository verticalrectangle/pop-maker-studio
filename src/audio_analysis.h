#pragma once
// Audio analysis v2 — every musical event the timeline can react to, for one audio file.
// Schema and algorithm requirements: docs/AUDIO_ANALYSIS.md. Times are SOURCE-file seconds;
// consumers map to timeline time through the audio clip that plays the file.
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

enum class HitKind : int { Kick = 0, Snare, Hat, Bass, Other, Vocal, Count };
enum class EnvKind : int { Mix = 0, Drums, Bass, Other, Vocals, Count };

const char* hit_kind_name(HitKind k);  // "kick" "snare" "hat" "bass" "other" "vocal"
const char* env_kind_name(EnvKind k);  // "mix" "drums" "bass" "other" "vocals"

// Event times are double: song-length sources put float32 time resolution at
// ~1e-5 s, enough to flip an event that lands exactly on a frame boundary
// (e.g. a downbeat at 94.300 s vs frame 378 at 60 fps after the clip offset).
struct AudioHit {
    double t = 0.0; // onset time, source seconds
    float s = 0.f;  // strength 0..1: peak / p90(picked peaks of this kind), clipped
};

struct AnalysisWord {
    std::string w;
    int   line = 0;       // index into AudioAnalysis::lines
    int   i    = 0;       // word index within the line
    double t0  = 0.0;     // source seconds
    double t1  = 0.0;
    float conf = 0.f;     // mean CTC token posterior, 0..1
};

struct AudioAnalysis {
    int         version = 2;
    std::string source;          // analysed audio file (absolute path)
    double      duration = 0.0;  // seconds
    float       bpm = 0.f;
    std::vector<double> beats;      // seconds, ascending
    std::vector<double> downbeats;  // seconds, ascending, subset of beats
    std::array<std::vector<AudioHit>, (int)HitKind::Count> hits;
    int fps = 60;                                                // envelope/spectrum frame rate
    std::array<std::vector<float>, (int)EnvKind::Count> env;     // 0..1 per frame (p98-normalised RMS)
    int spectrum_bands = 32;
    std::vector<uint8_t> spectrum;   // frames * spectrum_bands, row-major, 0..99
    std::vector<std::string>  lines;
    std::vector<AnalysisWord> words;
    std::array<std::string, 4> stems;  // drums, bass, other, vocals (wav paths; empty if not separated)
};

// JSON round-trip (docs/AUDIO_ANALYSIS.md). load accepts the reference timeline.json format.
bool        audio_analysis_load_json(const std::string& path, AudioAnalysis& out, std::string* err);
bool        audio_analysis_save_json(const AudioAnalysis& a, const std::string& path, std::string* err);
std::string audio_analysis_to_json(const AudioAnalysis& a);

struct AudioAnalysisOptions {
    bool separate_stems = true;          // 4-stem separation (drums/bass/other/vocals) when the model exists
    std::vector<std::string> lyrics;     // optional lyric lines to force-align to the vocals
    std::string stems_dir;               // reuse precomputed stems (<dir>/{drums,bass,other,vocals}.wav);
                                         // when all four exist, separation is skipped
};

// Blocking full analysis; run on a worker thread. progress(0..1, stage label).
bool audio_analysis_run(const std::string& audio_path, const AudioAnalysisOptions& opt,
                        AudioAnalysis& out, const std::function<void(float, const char*)>& progress,
                        std::string* err);
