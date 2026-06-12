#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <atomic>

// ── Audio FX parameters ───────────────────────────────────────────────────────
// Lives on every Audio/Video clip. All inactive by default (zero overhead).

struct AudioFX {
    // Autotune (YIN pitch detection + grain pitch shifting)
    bool  autotune_on    = false;
    int   autotune_key   = 0;       // root: 0=C 1=C# 2=D … 11=B
    int   autotune_scale = 0;       // 0=Major 1=Minor 2=Chromatic
    float autotune_speed = 0.f;     // 0=hard snap 100=gentle (ms)

    // Pitch shift (fixed ratio, no detection)
    bool  pitch_on        = false;
    float pitch_semitones = 0.f;    // -24 … +24

    // Formant shift (colours voice character independently of pitch)
    bool  formant_on    = false;
    float formant_shift = 0.f;      // -1 … +1 (in octave units)

    // Delay
    bool  delay_on       = false;
    float delay_time     = 0.300f;  // seconds (0.01 – 2.0)
    float delay_feedback = 0.40f;   // 0 – 0.95
    float delay_mix      = 0.30f;   // 0 – 1

    // Reverb (Freeverb-inspired comb+allpass)
    bool  reverb_on   = false;
    float reverb_room = 0.50f;      // room size 0 – 1
    float reverb_damp = 0.50f;      // HF damping 0 – 1
    float reverb_mix  = 0.30f;      // dry/wet 0 – 1

    // AI voice conversion (ML — requires model)
    bool        voice_convert_on  = false;
    std::string voice_model_path; // path to .pth or .onnx
    // Transpose into the target's register. Auto probes the octave
    // empirically ({-12,0,+12} test syntheses, best pitch tracking wins);
    // the slider is a manual offset applied on top.
    bool        voice_pitch_auto      = true;
    int         voice_pitch_semitones = 0;   // -24 … +24

    bool any_active() const {
        return autotune_on || pitch_on || formant_on ||
               delay_on || reverb_on || voice_convert_on;
    }
};

uint64_t audio_fx_hash(const AudioFX& fx);

// ── Streaming chain ───────────────────────────────────────────────────────────
// Ordered AudioFX stages as a live per-sample processor — the SAME DSP the
// offline bake runs, so live monitoring, preview and export can't diverge.
// create on a control thread; process is RT-safe (no locks, no allocs).
// Voice conversion is offline-only and ignored by the chain.
struct AudioFXChain;
AudioFXChain* audio_fx_chain_create(const std::vector<AudioFX>& stages, float sample_rate);
void          audio_fx_chain_process(AudioFXChain* c, float& L, float& R);
void          audio_fx_chain_free(AudioFXChain* c);

// Windowed variant for live clip playback (DAW-style): units only process
// inside their window in SOURCE seconds, with ~12 ms edge fades; a
// non-consecutive frame_idx (seek, loop wrap) wipes state RT-safely.
struct AudioFXSegment;
AudioFXChain* audio_fx_chain_create_seg(const std::vector<AudioFXSegment>& segs,
                                        float sample_rate);
void          audio_fx_chain_process_seg(AudioFXChain* c, float& L, float& R,
                                         float src_t, int64_t frame_idx);

// ── Windowed FX segments ──────────────────────────────────────────────────────
// An audio FX brick applies only over its own timeline range. Each segment is
// a window in SOURCE time (seconds into the decoded PCM) with the FX active
// inside it; outside, the audio stays dry. Stacked effects on one range come
// from MultiFX chains (one segment per chain entry).

struct AudioFXSegment {
    float   t0 = 0.f, t1 = 0.f;  // source-time window, seconds
    AudioFX fx;
};

uint64_t audio_fx_segments_hash(const std::vector<AudioFXSegment>& segs);

// ── Voice presets ─────────────────────────────────────────────────────────────

struct VoicePresetDef {
    const char* name;
    const char* description;
    AudioFX     fx;
};

extern const VoicePresetDef k_voice_presets[];
extern const int             k_voice_preset_count;

// ── Offline processor ─────────────────────────────────────────────────────────
// Runs on a background thread. raw = interleaved stereo f32 @ 44100.
// Returns processed interleaved stereo f32 @ 44100.

struct AudioFXJob {
    std::vector<float>      raw;
    AudioFX                 fx;
    std::atomic<uint64_t>   generation{0};  // bump to cancel in-flight job
};

std::vector<float> process_audio_fx(const std::vector<float>& raw,
                                    const AudioFX& fx,
                                    float sample_rate = 44100.f,
                                    const std::atomic<uint64_t>* cancel_gen = nullptr,
                                    uint64_t my_gen = 0);

// Windowed variant: each segment processes with ~1 s of pre-roll (detectors
// and delay lines are warm at the window start) and crossfades into the dry
// signal at both edges. Returns {} when cancelled.
std::vector<float> process_audio_fx_segments(const std::vector<float>& raw,
                                             const std::vector<AudioFXSegment>& segs,
                                             float sample_rate = 44100.f,
                                             const std::atomic<uint64_t>* cancel_gen = nullptr,
                                             uint64_t my_gen = 0);

// ── TTS status ────────────────────────────────────────────────────────────────

enum class TTSStatus { Idle, Running, Done, Error };

struct TTSState {
    TTSStatus   status   = TTSStatus::Idle;
    float       progress = 0.f;
    std::string out_path;
    std::string error;
};

void tts_generate(const std::string& text, const std::string& voice,
                  TTSState& out_state);

