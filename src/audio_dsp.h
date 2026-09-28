#pragma once
// Shared DSP for audio analysis v2 (docs/AUDIO_ANALYSIS.md).
//
// Bit-faithful C++ ports of the librosa algorithms the reference pipeline
// (~/Projects/seen-and-not-seen/pipeline/audio.py) uses: centered/left STFT,
// Slaney mel + power/amplitude dB, mel onset strength, band flux, greedy
// peak picking, Ellis DP beat tracking, tempo prior, piptrack tuning, STFT
// chroma, beat-sync. All conventions verified against librosa 1.0:
//
//   - hann windows are PERIODIC (scipy get_window fftbins=True)
//   - STFT magnitudes are raw FFT magnitudes (no 1/N scaling)
//   - power_to_db: 10*log10(max(S,1e-10)), ref=1.0 (librosa default), top_db=80 below the max
//   - amplitude_to_db: 20*log10(max(S,1e-5)), ref=1.0, top_db=80 below the max
//   - onset_strength (mel default): 128 mel bands, n_fft 2048, lag 1,
//     max_size 1, pad lag + n_fft/(2*hop), trim to input frames
//   - peak_pick greedy: exact edge-clamped slices, x[n]==max exact compare
//   - beat DP: tightness penalty, round-half-even, localmax tail select
//   - chroma: n_fft 2048, estimated tuning, l2 column norm after sync
//
// Consumers: src/beat_detect.cpp (mix beat tracking), src/audio_analysis_run.cpp.
#include <string>
#include <vector>

namespace aadsp {

// ── Audio decode ─────────────────────────────────────────────────────────────
// Decode any ffmpeg-readable file to mono f32 at out_sr (libswresample does
// the resampling/downmix). Returns false with err set on failure.
bool decode_mono(const std::string& path, int out_sr, std::vector<float>& pcm,
                 std::string* err);
// Decode only [t0, t1) (source seconds) to mono f32 at out_sr. Sample-accurate
// to the resampled grid: span boundaries round to the nearest output sample
// and times stay absolute (frame f ↔ (start_sample + f) / out_sr). Used by
// analysis ranges so a segment analysis matches the full-file analysis
// sample-for-sample inside the span.
bool decode_mono_span(const std::string& path, int out_sr, double t0, double t1,
                      std::vector<float>& pcm, std::string* err);

// ── Windows / STFT ───────────────────────────────────────────────────────────
std::vector<float> hann_periodic(int n);

// STFT magnitude. Layout: mag[f * nfr + t], f = 0..n_fft/2, raw FFT magnitudes.
// center=true: zero-pad n_fft/2 on both sides (librosa default), nfr = 1 + n/hop.
// center=false: left-aligned frames, nfr = 1 + floor((n - n_fft)/hop), <= 0 → empty.
void stft_mag(const float* y, int n, int n_fft, int hop, bool center,
              const float* win, std::vector<float>& mag, int& nfr);
inline int stft_bins(int n_fft) { return n_fft / 2 + 1; }

// ── Mel / dB ─────────────────────────────────────────────────────────────────
struct MelFB {
    int n_mels = 0, bins = 0;
    std::vector<float> w;  // row-major n_mels x bins (Slaney area-normalised)
};
MelFB make_mel(int sr, int n_fft, int n_mels, float fmin, float fmax);
// M = FB × power spectrum. M[m * nfr + t].
void mel_power(const MelFB& fb, const float* mag, int nfr, std::vector<float>& m);
void power_to_db_inplace(std::vector<float>& s);      // ref=1.0, floor = max - 80 dB
void amplitude_to_db_inplace(std::vector<float>& s);  // ref=1.0, floor = max - 80 dB

// ── Onset envelopes ──────────────────────────────────────────────────────────
// Default mel onset strength of a waveform (128 mel, n_fft 2048). Length nfr.
std::vector<float> onset_strength_mel(const float* y, int n, int sr, int hop);
// Onset strength from a dB mel spectrogram (lag 1, max_size 1, librosa S-path
// centering pad = 1 + 2048/(2*hop)). Length nfr.
std::vector<float> onset_strength_from_dbmel(const float* dbm, int n_mels, int nfr, int hop);
// Half-wave-rectified spectral flux (dB) restricted to [lo,hi) Hz, with the
// leading zero (np.r_[0.0, ...]). Length nfr.
std::vector<float> band_flux(const float* mag, int bins, int nfr, int sr, int n_fft,
                             float lo, float hi);

// ── Peak picking / stats ─────────────────────────────────────────────────────
std::vector<int> peak_pick(const float* x, int n, int pre_max, int post_max,
                           int pre_avg, int post_avg, float delta, int wait);
float percentile_sorted(std::vector<float> v, float q);  // sorts the copy (numpy linear)
float percentile(const float* x, int n, float q);
float median_of(const float* x, int n);

// ── Beat tracking (Ellis DP, librosa.beat.beat_track) ────────────────────────
struct BeatTrack {
    float bpm = 0.f;              // rounded to 2 decimals, 0 on failure
    std::vector<int> frames;      // onset-envelope frame indices, ascending
};
// trim=false (the reference keeps the trailing frame). tightness=400 like the reference.
BeatTrack beat_track(const float* oenv, int n, int sr, int hop,
                     float tightness = 400.f, bool trim = false);

// ── Chroma / downbeats ───────────────────────────────────────────────────────
float estimate_tuning(const float* y, int n, int sr);  // piptrack, n_fft 2048
struct ChromaFB {
    int n_fft = 0;
    std::vector<float> w;  // 12 x (n_fft/2+1), float32
};
ChromaFB make_chroma(int sr, int n_fft, float tuning);
// C = FB × power spectrum + per-column inf-norm. C[c * nfr + t].
void chroma_from_power(const ChromaFB& fb, const float* mag, int nfr,
                       std::vector<float>& c);
// Median-sync columns of C (rows x nfr) between consecutive bounds.
// bounds has nb+1 entries (beat frames, padded with nfr at the end).
std::vector<float> sync_median(const float* c, int rows, int nfr,
                               const int* bounds, int nb);
// Bar phase 0..3 maximising bass-onset-at-beat + chroma novelty (spec §downbeats).
// bo = bass onset envelope, bf = beat frames (nbeats), chroma = 12 x nfr.
int downbeat_phase(const float* bo, int n, const int* bf, int nbeats,
                   const float* chroma, int nfr);

// ── Frame-rate envelopes / spectrum ──────────────────────────────────────────
// Time-domain RMS per frame, frame_length 2048, zero-padded to exactly n_frames.
std::vector<float> frame_rms(const float* y, int n, int frame_len, int hop, int n_frames);

}  // namespace aadsp
