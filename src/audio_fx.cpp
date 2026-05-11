#include "audio_fx.h"
#include <cmath>
#include <cstring>
#include <algorithm>
#include <thread>
#include <cstdio>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <functional>
#include <filesystem>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ── Hash ─────────────────────────────────────────────────────────────────────

uint64_t audio_fx_hash(const AudioFX& fx) {
    // FNV-1a over the POD fields
    auto h = [](uint64_t acc, const void* data, size_t n) {
        const uint8_t* p = (const uint8_t*)data;
        for (size_t i = 0; i < n; ++i)
            acc = (acc ^ p[i]) * 1099511628211ULL;
        return acc;
    };
    uint64_t acc = 14695981039346656037ULL;
    acc = h(acc, &fx.autotune_on,    sizeof(fx.autotune_on));
    acc = h(acc, &fx.autotune_key,   sizeof(fx.autotune_key));
    acc = h(acc, &fx.autotune_scale, sizeof(fx.autotune_scale));
    acc = h(acc, &fx.autotune_speed, sizeof(fx.autotune_speed));
    acc = h(acc, &fx.pitch_on,        sizeof(fx.pitch_on));
    acc = h(acc, &fx.pitch_semitones, sizeof(fx.pitch_semitones));
    acc = h(acc, &fx.formant_on,    sizeof(fx.formant_on));
    acc = h(acc, &fx.formant_shift, sizeof(fx.formant_shift));
    acc = h(acc, &fx.delay_on,       sizeof(fx.delay_on));
    acc = h(acc, &fx.delay_time,     sizeof(fx.delay_time));
    acc = h(acc, &fx.delay_feedback, sizeof(fx.delay_feedback));
    acc = h(acc, &fx.delay_mix,      sizeof(fx.delay_mix));
    acc = h(acc, &fx.reverb_on,   sizeof(fx.reverb_on));
    acc = h(acc, &fx.reverb_room, sizeof(fx.reverb_room));
    acc = h(acc, &fx.reverb_damp, sizeof(fx.reverb_damp));
    acc = h(acc, &fx.reverb_mix,  sizeof(fx.reverb_mix));
    acc = h(acc, &fx.voice_convert_on, sizeof(fx.voice_convert_on));
    for (char c : fx.voice_model_path) acc = h(acc, &c, 1);
    return acc;
}

// ── Voice presets ─────────────────────────────────────────────────────────────

static AudioFX make_robot() {
    AudioFX f;
    f.autotune_on    = true;
    f.autotune_scale = 2;   // Chromatic — every semitone is valid
    f.autotune_speed = 0.f; // hard snap
    f.pitch_on        = true;
    f.pitch_semitones = -2.f;
    return f;
}
static AudioFX make_chipmunk() {
    AudioFX f;
    f.pitch_on        = true;
    f.pitch_semitones = 12.f;
    f.formant_on    = true;
    f.formant_shift = 0.5f;
    return f;
}
static AudioFX make_deep() {
    AudioFX f;
    f.pitch_on        = true;
    f.pitch_semitones = -7.f;
    f.formant_on    = true;
    f.formant_shift = -0.4f;
    return f;
}
static AudioFX make_radio() {
    AudioFX f;
    f.formant_on    = true;
    f.formant_shift = 0.25f;
    f.delay_on       = true;
    f.delay_time     = 0.018f;
    f.delay_feedback = 0.f;
    f.delay_mix      = 0.45f;
    return f;
}
static AudioFX make_demon() {
    AudioFX f;
    f.pitch_on        = true;
    f.pitch_semitones = -12.f;
    f.formant_on    = true;
    f.formant_shift = -0.7f;
    f.reverb_on   = true;
    f.reverb_room = 0.8f;
    f.reverb_damp = 0.3f;
    f.reverb_mix  = 0.25f;
    return f;
}

const VoicePresetDef k_voice_presets[] = {
    { "Robot",    "Hard chromatic snap, metallic pitch",  make_robot()    },
    { "Chipmunk", "+1 octave, bright formants",           make_chipmunk() },
    { "Deep",     "Rich deep voice, -7 semitones",        make_deep()     },
    { "Radio",    "Mid-range presence, slight echo",      make_radio()    },
    { "Demon",    "One octave down, cavernous reverb",    make_demon()    },
};
const int k_voice_preset_count = 5;

// ── GrainShifter ──────────────────────────────────────────────────────────────
// Ported from silvertune — stripped of CLAP, standalone C++.

struct GrainShifter {
    static constexpr uint32_t BUF_SIZE = 4096;
    static constexpr uint32_t MASK     = BUF_SIZE - 1;

    float    buf[BUF_SIZE] = {};
    uint32_t write_pos     = 0;
    double   phase_a       = 0.0;
    double   phase_b       = 0.5;
    uint32_t grain_size    = 256;

    void reset() {
        memset(buf, 0, sizeof(buf));
        write_pos = 0; phase_a = 0.0; phase_b = 0.5;
    }

    float process(float in, double pitch_ratio) {
        buf[write_pos & MASK] = in;
        ++write_pos;

        double phase_inc = (1.0 - pitch_ratio) / (double)grain_size;
        phase_a += phase_inc; phase_a -= std::floor(phase_a);
        phase_b += phase_inc; phase_b -= std::floor(phase_b);

        auto lerp_read = [&](double phase) -> float {
            double delay = phase * grain_size + 2.0;
            double rp = (double)write_pos - delay;
            double wr = fmod(rp, (double)(BUF_SIZE));
            if (wr < 0) wr += BUF_SIZE;
            uint32_t i0 = (uint32_t)wr & MASK;
            uint32_t i1 = (i0 + 1) & MASK;
            float frac = (float)(wr - std::floor(wr));
            return buf[i0] * (1.f - frac) + buf[i1] * frac;
        };

        float gain_a = 0.5f * (1.f - (float)std::cos(2.0 * M_PI * phase_a));
        float gain_b = 0.5f * (1.f - (float)std::cos(2.0 * M_PI * phase_b));
        return lerp_read(phase_a) * gain_a + lerp_read(phase_b) * gain_b;
    }
};

// ── YIN pitch detector ────────────────────────────────────────────────────────
// Adapted from silvertune for offline (block) processing.

static float yin_detect(const float* buf, int n) {
    // n must be even; analyzes first n/2 lags
    int half = n / 2;
    std::vector<float> d(half);

    // Difference function
    for (int tau = 0; tau < half; ++tau) {
        float sum = 0.f;
        for (int j = 0; j < half; ++j) {
            float delta = buf[j] - buf[j + tau];
            sum += delta * delta;
        }
        d[tau] = sum;
    }

    // Cumulative mean normalized difference
    d[0] = 1.f;
    float run = 0.f;
    for (int tau = 1; tau < half; ++tau) {
        run += d[tau];
        d[tau] = d[tau] * (float)tau / run;
    }

    // Absolute threshold
    int tau_est = 0;
    for (int tau = 2; tau < half; ++tau) {
        if (d[tau] < 0.15f) {
            while (tau + 1 < half && d[tau + 1] < d[tau]) ++tau;
            tau_est = tau;
            break;
        }
    }
    return tau_est;
}

// ── Scale quantizer ───────────────────────────────────────────────────────────
// Direct port of silvertune/src/scale.cpp.

static const bool k_major[12] = { true,false,true,false,true,true,false,true,false,true,false,true };
static const bool k_minor[12] = { true,false,true,true,false,true,false,true,true,false,true,false };

static float hz_to_midi(float hz) {
    if (hz <= 0.f) return 0.f;
    return 69.f + 12.f * std::log2(hz / 440.f);
}
static float midi_to_hz(float midi) {
    return 440.f * std::pow(2.f, (midi - 69.f) / 12.f);
}
static int quantize_midi(int midi, int root, int scale) {
    if (scale == 2) return midi; // chromatic
    const bool* pat = (scale == 0) ? k_major : k_minor;
    int nc = ((midi % 12) - root + 12) % 12;
    int ob = midi - nc;
    if (pat[nc]) return midi;
    for (int off = 1; off <= 6; ++off) {
        bool u = pat[(nc + off) % 12];
        bool dn = pat[(nc - off + 12) % 12];
        if (u && dn) return ob + nc + off;
        if (u)  return ob + nc + off;
        if (dn) return ob + nc - off;
    }
    return midi;
}

// ── Delay ─────────────────────────────────────────────────────────────────────

struct StereoDelay {
    std::vector<float> bufl, bufr;
    size_t wp = 0;
    float  sr = 44100.f;
    float  prev_feedback_l = 0.f, prev_feedback_r = 0.f;

    void init(float sample_rate, float max_time_s = 2.5f) {
        sr = sample_rate;
        size_t n = (size_t)(max_time_s * sample_rate) + 4;
        bufl.assign(n, 0.f);
        bufr.assign(n, 0.f);
        wp = 0;
    }

    void process(float inL, float inR, float& outL, float& outR,
                 float time, float feedback, float mix) {
        size_t n = bufl.size();
        size_t delay_frames = std::min((size_t)(time * sr), n - 1);
        size_t rp = (wp + n - delay_frames) % n;

        float dl = bufl[rp];
        float dr = bufr[rp];
        bufl[wp] = inL + dl * feedback;
        bufr[wp] = inR + dr * feedback;
        wp = (wp + 1) % n;

        outL = inL * (1.f - mix) + dl * mix;
        outR = inR * (1.f - mix) + dr * mix;
    }
};

// ── Reverb (Freeverb-inspired) ────────────────────────────────────────────────
// 4 comb filters + 2 allpass per channel.

struct CombFilter {
    std::vector<float> buf;
    size_t wp    = 0;
    float  fback = 0.f;
    float  damp1 = 0.f, damp2 = 0.f;
    float  store = 0.f;

    void init(size_t n, float feedback, float damp) {
        buf.assign(n, 0.f);
        wp = 0; fback = feedback;
        damp1 = damp; damp2 = 1.f - damp; store = 0.f;
    }
    float process(float in) {
        float out = buf[wp];
        store = out * damp2 + store * damp1;
        buf[wp] = in + store * fback;
        if (++wp >= buf.size()) wp = 0;
        return out;
    }
};

struct AllpassFilter {
    std::vector<float> buf;
    size_t wp      = 0;
    float  fback   = 0.f;

    void init(size_t n) { buf.assign(n, 0.f); wp = 0; fback = 0.5f; }
    float process(float in) {
        float out = buf[wp];
        float v   = in + out * fback;
        buf[wp]   = v;
        if (++wp >= buf.size()) wp = 0;
        return out - v;
    }
};

struct Reverb {
    static constexpr int N_COMB    = 4;
    static constexpr int N_ALLPASS = 2;

    // Tunings scaled to 44100 Hz (Freeverb reference)
    static constexpr size_t COMB_L[N_COMB]    = { 1116, 1188, 1277, 1356 };
    static constexpr size_t ALLPASS_L[N_ALLPASS] = { 225, 341 };

    CombFilter    comb_l[N_COMB],    comb_r[N_COMB];
    AllpassFilter allpass_l[N_ALLPASS], allpass_r[N_ALLPASS];

    void init(float room, float damp) {
        float fback = 0.70f + room * 0.28f;
        float d     = damp * 0.4f;
        for (int i = 0; i < N_COMB; ++i) {
            comb_l[i].init(COMB_L[i],     fback, d);
            comb_r[i].init(COMB_L[i] + 23, fback, d);
        }
        for (int i = 0; i < N_ALLPASS; ++i) {
            allpass_l[i].init(ALLPASS_L[i]);
            allpass_r[i].init(ALLPASS_L[i] + 19);
        }
    }

    void process(float inL, float inR, float& outL, float& outR) {
        float mixed = (inL + inR) * 0.5f;
        outL = 0.f; outR = 0.f;
        for (int i = 0; i < N_COMB; ++i) {
            outL += comb_l[i].process(mixed);
            outR += comb_r[i].process(mixed);
        }
        for (int i = 0; i < N_ALLPASS; ++i) {
            outL = allpass_l[i].process(outL);
            outR = allpass_r[i].process(outR);
        }
    }
};

// ── Autotune state (offline per-clip) ────────────────────────────────────────

struct ATState {
    static constexpr int WIN = 1024;
    static constexpr int HOP = 128;

    float held_ratio  = 1.f;
    float cur_ratio   = 1.f;
    int   low_conf    = 0;
    float win_buf[WIN] = {};
    int   wp          = 0;
    float sr          = 44100.f;

    void init(float sample_rate) { sr = sample_rate; wp = 0; memset(win_buf, 0, sizeof(win_buf)); }

    float push(float mono, float speed_ms, int root, int scale) {
        win_buf[wp % WIN] = mono;
        ++wp;
        if (wp % HOP == 0) {
            float tmp[WIN];
            for (int i = 0; i < WIN; ++i) tmp[i] = win_buf[(wp + i) % WIN];
            int tau = (int)yin_detect(tmp, WIN);
            if (tau > 0) {
                float det_hz  = sr / (float)tau;
                float det_midi = hz_to_midi(det_hz);
                int   qi       = quantize_midi((int)std::round(det_midi), root, scale);
                held_ratio = midi_to_hz((float)qi) / det_hz;
                low_conf = 0;
            } else {
                if (++low_conf > 8) held_ratio = 1.f;
            }
        }
        float chase_k = (speed_ms <= 0.f) ? 1.f
            : std::exp(-1.f / (speed_ms * 0.001f * sr));
        cur_ratio = cur_ratio * chase_k + held_ratio * (1.f - chase_k);
        return cur_ratio;
    }
};

// ── Main offline processor ────────────────────────────────────────────────────

std::vector<float> process_audio_fx(const std::vector<float>& raw,
                                    const AudioFX& fx,
                                    float sample_rate,
                                    const std::atomic<uint64_t>* cancel_gen,
                                    uint64_t my_gen)
{
    if (raw.empty() || !fx.any_active()) return raw;

    const int n_frames = (int)(raw.size() / 2);
    std::vector<float> out(raw.size());

    // Semitone-to-ratio helper
    auto st_ratio = [](float st) -> double {
        return std::pow(2.0, (double)st / 12.0);
    };

    // Grain shifters: [0]=L pitch, [1]=R pitch, [2]=L formant, [3]=R formant
    GrainShifter gs[4];
    for (auto& g : gs) g.reset();

    // Combined pitch ratio for grain shifters
    double pitch_ratio   = 1.0;
    double formant_ratio = 1.0;
    if (fx.pitch_on)   pitch_ratio   = st_ratio(fx.pitch_semitones);
    if (fx.formant_on) formant_ratio = st_ratio(fx.formant_shift * 12.f);

    ATState at;
    if (fx.autotune_on) at.init(sample_rate);

    StereoDelay delay;
    if (fx.delay_on) delay.init(sample_rate);

    Reverb reverb;
    if (fx.reverb_on) reverb.init(fx.reverb_room, fx.reverb_damp);

    for (int f = 0; f < n_frames; ++f) {
        // Cancellation check every 4096 frames
        if ((f & 0xFFF) == 0 && cancel_gen && cancel_gen->load() != my_gen)
            return {};  // cancelled

        float L = raw[f * 2];
        float R = raw[f * 2 + 1];

        // ── Autotune ────────────────────────────────────────────────────────
        if (fx.autotune_on) {
            float mono    = (L + R) * 0.5f;
            float at_rat  = at.push(mono, fx.autotune_speed,
                                    fx.autotune_key, fx.autotune_scale);
            double ratio  = (double)at_rat * pitch_ratio;
            L = gs[0].process(L, ratio);
            R = gs[1].process(R, ratio);
        } else if (fx.pitch_on) {
            // ── Pitch shift only ─────────────────────────────────────────────
            L = gs[0].process(L, pitch_ratio);
            R = gs[1].process(R, pitch_ratio);
        }

        // ── Formant shift (second grain pass) ───────────────────────────────
        if (fx.formant_on) {
            L = gs[2].process(L, formant_ratio);
            R = gs[3].process(R, formant_ratio);
        }

        // ── Delay ────────────────────────────────────────────────────────────
        if (fx.delay_on) {
            float dL, dR;
            delay.process(L, R, dL, dR, fx.delay_time, fx.delay_feedback, fx.delay_mix);
            L = dL; R = dR;
        }

        // ── Reverb ────────────────────────────────────────────────────────────
        if (fx.reverb_on) {
            float rL, rR;
            reverb.process(L, R, rL, rR);
            L = L * (1.f - fx.reverb_mix) + rL * fx.reverb_mix;
            R = R * (1.f - fx.reverb_mix) + rR * fx.reverb_mix;
        }

        out[f * 2]     = std::fmaxf(-1.f, std::fminf(1.f, L));
        out[f * 2 + 1] = std::fmaxf(-1.f, std::fminf(1.f, R));
    }

    // ── Voice conversion (RVC) ────────────────────────────────────────────────
    if (fx.voice_convert_on && !fx.voice_model_path.empty()
        && !fx.vc_python_path.empty() && !fx.vc_script_path.empty()
        && std::filesystem::exists(fx.voice_model_path)) {

        // Write temp input WAV (IEEE float 32-bit, stereo, 44100 Hz)
        std::string key = fx.voice_model_path
                        + std::to_string(out.size());
        uint64_t h64 = std::hash<std::string>{}(key);
        std::string tmp_in  = "/tmp/pms_vcin_"  + std::to_string(h64) + ".wav";
        std::string tmp_out = "/tmp/pms_vcout_" + std::to_string(h64) + ".wav";

        // Write IEEE float WAV
        if (FILE* f = fopen(tmp_in.c_str(), "wb")) {
            uint32_t data_bytes  = (uint32_t)(out.size() * sizeof(float));
            uint32_t riff_size   = 4 + 8 + 18 + 8 + data_bytes;
            uint16_t fmt         = 3;    // IEEE_FLOAT
            uint16_t channels    = 2;
            uint32_t sample_rate = 44100;
            uint16_t bits        = 32;
            uint16_t block_align = channels * (bits / 8);
            uint32_t byte_rate   = sample_rate * block_align;
            uint16_t ext_size    = 0;
            fwrite("RIFF", 1, 4, f);  fwrite(&riff_size,   4, 1, f);
            fwrite("WAVE", 1, 4, f);
            fwrite("fmt ", 1, 4, f);  uint32_t fmt_size = 18;
            fwrite(&fmt_size,   4, 1, f); fwrite(&fmt,        2, 1, f);
            fwrite(&channels,   2, 1, f); fwrite(&sample_rate,4, 1, f);
            fwrite(&byte_rate,  4, 1, f); fwrite(&block_align,2, 1, f);
            fwrite(&bits,       2, 1, f); fwrite(&ext_size,   2, 1, f);
            fwrite("data", 1, 4, f);  fwrite(&data_bytes,  4, 1, f);
            fwrite(out.data(), sizeof(float), out.size(), f);
            fclose(f);

            // Run voice_convert.py (blocking — we're already in a worker thread)
            std::string cmd = "\"" + fx.vc_python_path + "\" \""
                            + fx.vc_script_path + "\" \""
                            + tmp_in + "\" \""
                            + fx.voice_model_path + "\" \""
                            + tmp_out + "\" 2>/dev/null";
            system(cmd.c_str());  // NOLINT

            // Read back result via ffmpeg → f32le 44100 stereo
            if (std::filesystem::exists(tmp_out)) {
                std::string raw_tmp = tmp_out + ".raw";
                std::string dec_cmd = "ffmpeg -hide_banner -loglevel error -y"
                                      " -i \"" + tmp_out + "\""
                                      " -vn -ar 44100 -ac 2 -f f32le \""
                                      + raw_tmp + "\" 2>/dev/null";
                if (system(dec_cmd.c_str()) == 0) {  // NOLINT
                    if (FILE* rf = fopen(raw_tmp.c_str(), "rb")) {
                        fseek(rf, 0, SEEK_END);
                        long sz = ftell(rf); rewind(rf);
                        if (sz > 0) {
                            std::vector<float> converted(sz / sizeof(float));
                            fread(converted.data(), sizeof(float), converted.size(), rf);
                            out = std::move(converted);
                        }
                        fclose(rf);
                    }
                    std::filesystem::remove(raw_tmp);
                }
                std::filesystem::remove(tmp_out);
            }
            std::filesystem::remove(tmp_in);
        }
    }

    return out;
}

// ── TTS ───────────────────────────────────────────────────────────────────────

void tts_generate(const std::string& text, const std::string& voice,
                  const std::string& python_path, const std::string& script_path,
                  TTSState& out_state)
{
    out_state.status   = TTSStatus::Running;
    out_state.progress = 0.f;
    out_state.error.clear();

    std::string out_path = "/tmp/pms_tts_" +
        std::to_string(std::hash<std::string>{}(text + voice)) + ".wav";
    out_state.out_path = out_path;

    std::thread([=, &out_state]() {
        std::string cmd = "\"" + python_path + "\" \"" + script_path + "\" "
                        + "\"" + text + "\" "
                        + "\"" + voice + "\" "
                        + "\"" + out_path + "\" 2>&1";
        FILE* p = popen(cmd.c_str(), "r");
        if (!p) {
            out_state.error  = "Failed to launch TTS";
            out_state.status = TTSStatus::Error;
            return;
        }
        char buf[256];
        while (fgets(buf, sizeof(buf), p)) {
            float prog = 0.f;
            if (sscanf(buf, "PROGRESS %f", &prog) == 1)
                out_state.progress = prog;
        }
        int rc = pclose(p);
        if (rc != 0) {
            out_state.error  = "TTS script failed";
            out_state.status = TTSStatus::Error;
        } else {
            out_state.progress = 1.f;
            out_state.status   = TTSStatus::Done;
        }
    }).detach();
}

// ── Voice conversion ─────────────────────────────────────────────────────────

static void run_subprocess(const std::string& cmd, VCState& out_state)
{
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) {
        out_state.error  = "Failed to launch subprocess";
        out_state.status = VCStatus::Error;
        return;
    }
    char buf[512];
    while (fgets(buf, sizeof(buf), p)) {
        float prog = 0.f;
        if (sscanf(buf, "PROGRESS %f", &prog) == 1)
            out_state.progress = prog;
        else if (strncmp(buf, "ERROR", 5) == 0) {
            out_state.error = buf;
            out_state.error.erase(out_state.error.find_last_not_of("\r\n") + 1);
        }
    }
    int rc = pclose(p);
    if (rc != 0 && out_state.status != VCStatus::Error) {
        if (out_state.error.empty()) out_state.error = "Process exited with error";
        out_state.status = VCStatus::Error;
    } else if (rc == 0) {
        out_state.progress = 1.f;
        out_state.status   = VCStatus::Done;
    }
}

void vc_convert(const std::string& input_wav, const std::string& model_path,
                const std::string& python_path, const std::string& script_path,
                VCState& out_state)
{
    out_state.status   = VCStatus::Running;
    out_state.progress = 0.f;
    out_state.error.clear();

    std::string out_path = "/tmp/pms_vc_" +
        std::to_string(std::hash<std::string>{}(input_wav + model_path)) + ".wav";
    out_state.out_path = out_path;

    std::thread([=, &out_state]() {
        std::string cmd = "\"" + python_path + "\" \"" + script_path + "\" "
                        + "\"" + input_wav  + "\" "
                        + "\"" + model_path + "\" "
                        + "\"" + out_path   + "\" 2>&1";
        run_subprocess(cmd, out_state);
    }).detach();
}

void vc_download(const std::string& hf_repo, const std::string& hf_filename,
                 const std::string& out_path,
                 const std::string& python_path, const std::string& script_path,
                 VCState& out_state)
{
    out_state.status   = VCStatus::Running;
    out_state.progress = 0.f;
    out_state.error.clear();
    out_state.out_path = out_path;

    std::thread([=, &out_state]() {
        std::string cmd = "\"" + python_path + "\" \"" + script_path + "\" "
                        + "--download "
                        + "\"" + hf_repo     + "\" "
                        + "\"" + hf_filename + "\" "
                        + "\"" + out_path    + "\" 2>&1";
        run_subprocess(cmd, out_state);
    }).detach();
}
