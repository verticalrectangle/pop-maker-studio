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
    std::string voice_model_path; // path to .pth

    bool any_active() const {
        return autotune_on || pitch_on || formant_on ||
               delay_on || reverb_on || voice_convert_on;
    }
};

uint64_t audio_fx_hash(const AudioFX& fx);

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

