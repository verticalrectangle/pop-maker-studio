// Shared DSP for audio analysis v2 — librosa-faithful primitives.
// Conventions (verified against librosa 1.0, see audio_dsp.h): periodic Hann,
// raw-FFT magnitudes, ref=max + top_db=80 dB maps, greedy edge-clamped peak
// picking, Ellis DP beat tracking with librosa's exact rounding/padding/trim.
#include "audio_dsp.h"

#include "platform.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>

#if PMS_HAS_FFMPEG
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}
#endif
#if PMS_HAS_FFTW
#include <fftw3.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace aadsp {

#if PMS_HAS_FFMPEG
bool decode_mono(const std::string& path, int out_sr, std::vector<float>& pcm,
                 std::string* err) {
    pcm.clear();
    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, path.c_str(), nullptr, nullptr) < 0) {
        if (err) *err = "cannot open audio: " + path;
        return false;
    }
    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        avformat_close_input(&fmt_ctx);
        if (err) *err = "cannot probe audio: " + path;
        return false;
    }
    int audio_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audio_idx < 0) {
        avformat_close_input(&fmt_ctx);
        if (err) *err = "no audio stream: " + path;
        return false;
    }
    AVStream* stream = fmt_ctx->streams[audio_idx];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        avformat_close_input(&fmt_ctx);
        if (err) *err = "no decoder: " + path;
        return false;
    }
    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codec_ctx, stream->codecpar);
    if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        if (err) *err = "cannot open decoder: " + path;
        return false;
    }
    SwrContext* swr = swr_alloc();
#if LIBAVUTIL_VERSION_MAJOR >= 57
    av_opt_set_chlayout(swr, "in_chlayout", &codec_ctx->ch_layout, 0);
    av_opt_set_int(swr, "in_sample_rate", codec_ctx->sample_rate, 0);
    av_opt_set_sample_fmt(swr, "in_sample_fmt", codec_ctx->sample_fmt, 0);
    AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
    av_opt_set_chlayout(swr, "out_chlayout", &mono, 0);
#else
    av_opt_set_int(swr, "in_channel_count", codec_ctx->channels, 0);
    av_opt_set_int(swr, "in_channel_layout", (int64_t)codec_ctx->channel_layout, 0);
    av_opt_set_int(swr, "in_sample_rate", codec_ctx->sample_rate, 0);
    av_opt_set_sample_fmt(swr, "in_sample_fmt", codec_ctx->sample_fmt, 0);
    av_opt_set_int(swr, "out_channel_count", 1, 0);
    av_opt_set_int(swr, "out_channel_layout", AV_CH_LAYOUT_MONO, 0);
#endif
    av_opt_set_int(swr, "out_sample_rate", out_sr, 0);
    av_opt_set_sample_fmt(swr, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);
    swr_init(swr);

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == audio_idx) {
            avcodec_send_packet(codec_ctx, pkt);
            while (avcodec_receive_frame(codec_ctx, frame) == 0) {
                int out_samples = (int)av_rescale_rnd(
                    swr_get_delay(swr, codec_ctx->sample_rate) + frame->nb_samples,
                    out_sr, codec_ctx->sample_rate, AV_ROUND_UP);
                std::vector<float> buf((size_t)out_samples);
                uint8_t* out_ptr = (uint8_t*)buf.data();
                int got = swr_convert(swr, &out_ptr, out_samples,
                                      (const uint8_t**)frame->data, frame->nb_samples);
                if (got > 0) pcm.insert(pcm.end(), buf.begin(), buf.begin() + got);
            }
        }
        av_packet_unref(pkt);
    }
    {
        int tail = (int)av_rescale_rnd(swr_get_delay(swr, codec_ctx->sample_rate),
                                       out_sr, codec_ctx->sample_rate, AV_ROUND_UP);
        if (tail > 0) {
            std::vector<float> buf((size_t)tail);
            uint8_t* out_ptr = (uint8_t*)buf.data();
            int got = swr_convert(swr, &out_ptr, tail, nullptr, 0);
            if (got > 0) pcm.insert(pcm.end(), buf.begin(), buf.begin() + got);
        }
    }
    av_frame_free(&frame);
    av_packet_free(&pkt);
    swr_free(&swr);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    if (pcm.empty()) {
        if (err) *err = "decoded no samples: " + path;
        return false;
    }
    return true;
}
#else
bool decode_mono(const std::string&, int, std::vector<float>&, std::string* err) {
    if (err) *err = "audio decode unavailable in this build";
    return false;
}
#endif

std::vector<float> hann_periodic(int n) {
    std::vector<float> w((size_t)n);
    for (int i = 0; i < n; i++)
        w[i] = 0.5f * (1.f - cosf(2.f * (float)M_PI * i / n));
    return w;
}

#if PMS_HAS_FFTW
namespace {
// One FFTW plan per (n_fft) — plans are NOT thread-safe, so serialise them.
// Rows are processed under the same lock (FFTW has no per-row plan state).
std::mutex& fft_mutex() {
    static std::mutex m;
    return m;
}
struct FftCtx {
    int n_fft = 0;
    fftwf_plan plan = nullptr;
    float* in = nullptr;
    fftwf_complex* out = nullptr;
    ~FftCtx() {
        if (plan) fftwf_destroy_plan(plan);
        if (in) fftwf_free(in);
        if (out) fftwf_free(out);
    }
};
std::shared_ptr<FftCtx> fft_ctx(int n_fft) {
    static std::mutex m;
    static std::map<int, std::weak_ptr<FftCtx>> cache;
    std::lock_guard<std::mutex> lk(m);
    auto it = cache.find(n_fft);
    if (it != cache.end()) {
        if (auto p = it->second.lock()) return p;
    }
    auto c = std::make_shared<FftCtx>();
    std::lock_guard<std::mutex> fl(fft_mutex());
    c->n_fft = n_fft;
    c->in = fftwf_alloc_real(n_fft);
    c->out = fftwf_alloc_complex(n_fft / 2 + 1);
    c->plan = fftwf_plan_dft_r2c_1d(n_fft, c->in, c->out, FFTW_ESTIMATE);
    cache[n_fft] = c;
    return c;
}
}  // namespace
#endif

void stft_mag(const float* y, int n, int n_fft, int hop, bool center,
              const float* win, std::vector<float>& mag, int& nfr) {
    mag.clear();
    nfr = 0;
#if !PMS_HAS_FFTW
    (void)y;
    (void)n;
    (void)n_fft;
    (void)hop;
    (void)center;
    (void)win;
    return;
#else
    const int bins = stft_bins(n_fft);
    int nfr_loc = 0, pad = 0;
    if (center) {
        pad = n_fft / 2;
        nfr_loc = 1 + n / hop;
    } else {
        if (n < n_fft) return;
        nfr_loc = 1 + (n - n_fft) / hop;
    }
    mag.assign((size_t)bins * nfr_loc, 0.f);
    auto ctx = fft_ctx(n_fft);
    std::lock_guard<std::mutex> lk(fft_mutex());
    for (int t = 0; t < nfr_loc; t++) {
        int start = t * hop - pad;
        for (int i = 0; i < n_fft; i++) {
            int idx = start + i;
            ctx->in[i] = (idx >= 0 && idx < n) ? y[idx] * win[i] : 0.f;
        }
        fftwf_execute(ctx->plan);
        for (int f = 0; f < bins; f++) {
            float re = ctx->out[f][0], im = ctx->out[f][1];
            mag[(size_t)f * nfr_loc + t] = sqrtf(re * re + im * im);
        }
    }
    nfr = nfr_loc;
#endif
}

// ── Mel scale (Slaney) ───────────────────────────────────────────────────────

static double hz_to_mel_slaney(double f) {
    const double f_sp = 200.0 / 3;
    double m = f / f_sp;
    if (f >= 1000.0) m = 15.0 + log(f / 1000.0) / (log(6.4) / 27.0);
    return m;
}
static double mel_to_hz_slaney(double m) {
    const double f_sp = 200.0 / 3;
    if (m < 15.0) return m * f_sp;
    return 1000.0 * exp((log(6.4) / 27.0) * (m - 15.0));
}

MelFB make_mel(int sr, int n_fft, int n_mels, float fmin, float fmax) {
    MelFB fb;
    fb.n_mels = n_mels;
    fb.bins = stft_bins(n_fft);
    fb.w.assign((size_t)n_mels * fb.bins, 0.f);
    std::vector<double> mel_f((size_t)n_mels + 2);
    double mlo = hz_to_mel_slaney(fmin), mhi = hz_to_mel_slaney(fmax);
    for (int i = 0; i < n_mels + 2; i++)
        mel_f[i] = mel_to_hz_slaney(mlo + (mhi - mlo) * i / (n_mels + 1));
    std::vector<double> fdiff((size_t)n_mels + 1);
    for (int i = 0; i < n_mels + 1; i++) fdiff[i] = mel_f[i + 1] - mel_f[i];
    double step = (double)sr / n_fft;
    for (int i = 0; i < n_mels; i++) {
        for (int b = 0; b < fb.bins; b++) {
            double f = b * step;
            double lower = -(mel_f[i] - f) / fdiff[i];
            double upper = (mel_f[i + 2] - f) / fdiff[i + 1];
            double v = std::min(lower, upper);
            if (v > 0) fb.w[(size_t)i * fb.bins + b] = (float)v;
        }
        // Slaney area normalisation.
        double enorm = 2.0 / (mel_f[i + 2] - mel_f[i]);
        for (int b = 0; b < fb.bins; b++) fb.w[(size_t)i * fb.bins + b] *= (float)enorm;
    }
    return fb;
}

void mel_power(const MelFB& fb, const float* mag, int nfr, std::vector<float>& m) {
    m.assign((size_t)fb.n_mels * nfr, 0.f);
    for (int i = 0; i < fb.n_mels; i++) {
        const float* wrow = fb.w.data() + (size_t)i * fb.bins;
        for (int t = 0; t < nfr; t++) {
            double acc = 0.0;
            for (int b = 0; b < fb.bins; b++) {
                float v = mag[(size_t)b * nfr + t];
                acc += (double)wrow[b] * v * v;
            }
            m[(size_t)i * nfr + t] = (float)acc;
        }
    }
}

static void to_db_inplace(std::vector<float>& s, float mult, float amin, float top_db) {
    if (s.empty()) return;
    float mx = -1e30f;
    for (float& v : s) {
        float v2 = mult * log10f(fmaxf(amin, fabsf(v)));
        v = v2;
        if (v2 > mx) mx = v2;
    }
    float floor = mx - top_db;
    for (float& v : s)
        if (v < floor) v = floor;
}
void power_to_db_inplace(std::vector<float>& s) { to_db_inplace(s, 10.f, 1e-10f, 80.f); }
void amplitude_to_db_inplace(std::vector<float>& s) { to_db_inplace(s, 20.f, 1e-5f, 80.f); }

// ── Onset envelopes ──────────────────────────────────────────────────────────

std::vector<float> onset_strength_from_dbmel(const float* dbm, int n_mels, int nfr,
                                             int hop) {
    // librosa.onset.onset_strength_multi (max_size=1 → ref=S, lag=1):
    //   flux[t] = mean(max(0, S[t] − S[t−1])) for t ≥ 1 (m[0] = 0),
    //   then LEFT-padded by pad = lag (+ n_fft//(2*hop) when center) and
    //   trimmed: out[t] = m[t−pad] for t ≥ pad (zeros before). The lag term
    //   is the pad itself — the diff must NOT also shift (an earlier
    //   revision computed m[t] from S[t]−S[t−1] AND padded by lag+center,
    //   shifting everything one hop late vs librosa). Verified
    //   value-for-value against librosa 1.0 on the reference clip
    //   (o[14..17] = 1.66/8.05/7.40/4.86 on both). n_fft = 2048 always —
    //   librosa's S-path centering shift.
    std::vector<float> env((size_t)nfr);
    if (nfr == 0) return env;
    std::vector<float> m((size_t)nfr, 0.f);
    for (int t = 1; t < nfr; t++) {
        double acc = 0.0;
        for (int i = 0; i < n_mels; i++)
            acc += std::max(0.0, (double)dbm[(size_t)i * nfr + t] -
                                     (double)dbm[(size_t)i * nfr + t - 1]);
        m[t] = (float)(acc / n_mels);
    }
    int pad = 2048 / (2 * hop);
    std::vector<float> out((size_t)nfr, 0.f);
    for (int t = pad; t < nfr; t++) out[t] = m[t - pad];
    return out;
}

std::vector<float> onset_strength_mel(const float* y, int n, int sr, int hop) {
    const int n_fft = 2048, n_mels = 128;
    auto win = hann_periodic(n_fft);
    // Centered STFT (librosa default), nfr = 1 + n/hop.
    std::vector<float> mag;
    int nfr = 0;
    stft_mag(y, n, n_fft, hop, true, win.data(), mag, nfr);
    MelFB fb = make_mel(sr, n_fft, n_mels, 0.f, sr / 2.f);
    std::vector<float> m;
    mel_power(fb, mag.data(), nfr, m);
    power_to_db_inplace(m);
    return onset_strength_from_dbmel(m.data(), n_mels, nfr, hop);
}

std::vector<float> band_flux(const float* mag, int bins, int nfr, int sr, int n_fft,
                             float lo, float hi) {
    std::vector<float> env((size_t)nfr, 0.f);
    if (nfr == 0) return env;
    // dB of the selected band rows (amplitude_to_db, ref = band max).
    std::vector<int> rows;
    double step = (double)sr / n_fft;
    for (int b = 0; b < bins; b++) {
        double f = b * step;
        if (f >= lo && f < hi) rows.push_back(b);
    }
    if (rows.empty()) return env;
    float mx = 1e-10f;
    for (int b : rows)
        for (int t = 0; t < nfr; t++) mx = fmaxf(mx, mag[(size_t)b * nfr + t]);
    double acc0 = 0.0;
    for (int t = 0; t < nfr; t++) {
        double acc = 0.0;
        for (int b : rows) {
            float v = 20.f * log10f(fmaxf(1e-5f, mag[(size_t)b * nfr + t] / mx));
            float p = (t > 0) ? 20.f * log10f(fmaxf(1e-5f, mag[(size_t)b * nfr + t - 1] / mx))
                              : v;
            acc += std::max(0.0, (double)(v - p));
        }
        acc /= rows.size();
        if (t > 0) env[t] = (float)acc;
        (void)acc0;
    }
    return env;
}

// ── Peak picking / stats ─────────────────────────────────────────────────────

std::vector<int> peak_pick(const float* x, int n, int pre_max, int post_max,
                           int pre_avg, int post_avg, float delta, int wait) {
    std::vector<int> peaks;
    if (n <= 0) return peaks;
    // Frame 0 special case (edge-clamped slices).
    {
        int pmax = std::min(post_max, n);
        float mx = x[0];
        for (int i = 0; i < pmax; i++) mx = fmaxf(mx, x[i]);
        int pavg = std::min(post_avg, n);
        double s = 0;
        for (int i = 0; i < pavg; i++) s += x[i];
        double av = pavg ? s / pavg : 0;
        if (x[0] >= mx && x[0] >= (float)(av + delta)) peaks.push_back(0);
    }
    int i = peaks.empty() ? 1 : wait + 1;
    while (i < n) {
        int a0 = std::max(0, i - pre_max), a1 = std::min(n, i + post_max);
        float mx = x[a0];
        for (int k = a0; k < a1; k++) mx = fmaxf(mx, x[k]);
        if (x[i] != mx) {
            i++;
            continue;
        }
        int b0 = std::max(0, i - pre_avg), b1 = std::min(n, i + post_avg);
        double s = 0;
        for (int k = b0; k < b1; k++) s += x[k];
        double av = (b1 > b0) ? s / (b1 - b0) : 0;
        if (x[i] < (float)(av + delta)) {
            i++;
            continue;
        }
        peaks.push_back(i);
        i += wait + 1;
    }
    return peaks;
}

float percentile_sorted(std::vector<float> v, float q) {
    if (v.empty()) return 0.f;
    std::sort(v.begin(), v.end());
    double pos = (double)q * (v.size() - 1);
    size_t lo = (size_t)floor(pos);
    double frac = pos - lo;
    if (lo + 1 >= v.size()) return v.back();
    return (float)(v[lo] * (1 - frac) + v[lo + 1] * frac);
}
float percentile(const float* x, int n, float q) {
    if (n <= 0) return 0.f;
    std::vector<float> v(x, x + n);
    return percentile_sorted(std::move(v), q);
}
float median_of(const float* x, int n) { return percentile(x, n, 0.5f); }

// ── Beat tracking ────────────────────────────────────────────────────────────
// librosa.beat.beat_track, static-tempo path: median melspectrogram onset
// envelope → global autocorrelation tempo (8 s window, log-normal prior
// start_bpm=120, std=1, max 320) → gaussian-smoothed localscore → tightness DP
// → localmax tail select → backtrack → trim=off.

static std::vector<float> autocorr(const float* x, int n, int max_lag) {
    std::vector<float> ac((size_t)max_lag, 0.f);
    for (int lag = 0; lag < max_lag; lag++) {
        double s = 0;
        for (int i = 0; i + lag < n; i++) s += (double)x[i] * x[i + lag];
        ac[lag] = (float)s;
    }
    return ac;
}

static float estimate_tempo_global(const float* oenv, int n, int sr, int hop) {
    if (n < 8) return 0.f;
    const double ac_size = 8.0;
    int win_length = (int)(ac_size * sr / hop);
    if (win_length < 2) return 0.f;
    // Centered windows (linear_ramp pad), hop 1 over the envelope.
    int npad = win_length / 2;
    std::vector<float> padded((size_t)n + 2 * npad, 0.f);
    for (int i = 0; i < npad; i++) {
        float a = (float)(i + 1) / (npad + 1);
        padded[i] = oenv[0] * a;
        padded[(size_t)n + npad + i] = oenv[n - 1] * (1.f - a);
    }
    for (int i = 0; i < n; i++) padded[i + npad] = oenv[i];
    auto hann = hann_periodic(win_length);
    // Mean over windows of the windowed autocorrelation (inf-normalised per window).
    std::vector<double> tg((size_t)win_length, 0.0);
    for (int t = 0; t < n; t++) {
        std::vector<float> w((size_t)win_length);
        for (int k = 0; k < win_length; k++) w[k] = padded[t + k] * hann[k];
        auto ac = autocorr(w.data(), win_length, win_length);
        float mx = 0.f;
        for (float v : ac) mx = fmaxf(mx, fabsf(v));
        if (mx <= 0.f) continue;
        for (int k = 0; k < win_length; k++) tg[k] += ac[k] / mx;
    }
    for (double& v : tg) v /= n;
    // Prior-weighted argmax over lags >= 1 (best_period indexes the lag).
    double log2start = log2(120.0);
    int best = 1;
    double bestv = -1e300;
    for (int lag = 1; lag < win_length; lag++) {
        double bpm = 60.0 * sr / (hop * lag);
        if (!(bpm < 320.0)) continue;
        double logprior = -0.5 * pow((log2(bpm) - log2start) / 1.0, 2);
        double v = log1p(1e6 * tg[lag]) + logprior;
        if (v > bestv) {
            bestv = v;
            best = lag;
        }
    }
    return (float)(60.0 * sr / (hop * best));
}

BeatTrack beat_track(const float* oenv, int n, int sr, int hop, float tightness,
                     bool trim) {
    BeatTrack out;
    if (n <= 0) return out;
    bool any = false;
    for (int i = 0; i < n; i++)
        if (oenv[i] != 0.f) {
            any = true;
            break;
        }
    if (!any) return out;

    float bpm = estimate_tempo_global(oenv, n, sr, hop);
    if (!(bpm > 0.f)) return out;
    out.bpm = roundf(bpm * 100.f) / 100.f;

    double fpb = (double)sr * 60.0 / hop / bpm;
    double fpb_r = round(fpb);
    if (fpb_r < 1) fpb_r = 1;

    // localscore: same-mode gaussian convolution, sigma = fpb/32 (std, samples).
    int R = (int)fpb_r;
    int K = 2 * R + 1;
    std::vector<double> g((size_t)K);
    for (int k = 0; k < K; k++) {
        double d = (k - R) * 32.0 / fpb_r;
        g[k] = exp(-0.5 * d * d);
    }
    std::vector<double> ls((size_t)n, 0.0);
    {
        double mean = 0;
        for (int i = 0; i < n; i++) mean += oenv[i];
        mean /= n;
        double var = 0;
        for (int i = 0; i < n; i++) {
            double d = oenv[i] - mean;
            var += d * d;
        }
        double sd = (n > 1) ? sqrt(var / (n - 1)) : 0;
        double tiny = 1.1754944e-38;
        double den = sd + tiny;
        for (int i = 0; i < n; i++) {
            double acc = 0;
            int k0 = std::max(0, i + R - n + 1), k1 = std::min(i + R, K - 1);
            // k ranges with 0 <= i + R - k < n.
            for (int k = k0; k <= k1; k++) acc += g[k] * (oenv[i + R - k] / den);
            ls[i] = acc;
        }
    }

    // DP over predecessors in [i - round(fpb/2), i - 2*fpb - 1].
    std::vector<double> cum((size_t)n, 0.0);
    std::vector<int> back((size_t)n, -1);
    double score_thresh = 0.01 * *std::max_element(ls.begin(), ls.end());
    bool first = true;
    for (int i = 0; i < n; i++) {
        double best = -1e300;
        int bloc = -1;
        long lo = (long)i - llround(fpb / 2), hi = (long)i - (long)(2 * fpb) - 1;
        for (long loc = lo; loc >= hi; loc--) {
            if (loc < 0) break;
            double s = cum[loc] - tightness * pow(log((double)(i - loc)) - log(fpb), 2);
            if (s > best) {
                best = s;
                bloc = (int)loc;
            }
        }
        cum[i] = (bloc >= 0) ? ls[i] + best : ls[i];
        if (first && ls[i] < score_thresh) {
            back[i] = -1;
        } else {
            back[i] = bloc;
            first = false;
        }
    }
    // Tail: last localmax of cumscore above half the median of localmax values.
    std::vector<int> ismax((size_t)n, 0);
    for (int i = 1; i < n; i++) {
        bool l = cum[i] > cum[i - 1];
        bool r = (i + 1 >= n) ? true : (cum[i] >= cum[i + 1]);
        ismax[i] = (l && r) ? 1 : 0;
    }
    std::vector<double> lm;
    for (int i = 0; i < n; i++)
        if (ismax[i]) lm.push_back(cum[i]);
    double med = 0;
    if (!lm.empty()) {
        std::sort(lm.begin(), lm.end());
        size_t m = lm.size();
        med = (m % 2) ? lm[m / 2] : 0.5 * (lm[m / 2 - 1] + lm[m / 2]);
    }
    int tail = n - 1;
    for (int i = n - 1; i >= 0; i--) {
        if (ismax[i] && cum[i] >= 0.5 * med) {
            tail = i;
            break;
        }
    }
    std::vector<char> beats((size_t)n, 0);
    for (int i = tail; i >= 0;) {
        beats[i] = 1;
        int p = back[i];
        if (p < 0) break;
        i = p;
    }
    if (trim) {
        // librosa trims leading/trailing beats below half the RMS of the
        // hann(5)-smoothed beat envelope.
        std::vector<double> w = {0.0, 0.5, 1.0, 0.5, 0.0};
        std::vector<double> env;
        for (int i = 0; i < n; i++)
            if (beats[i]) env.push_back(ls[i]);
        std::vector<double> sm(env.size(), 0.0);
        for (size_t i = 0; i < env.size(); i++) {
            double acc = 0;
            for (int k = 0; k < 5; k++) {
                long j = (long)i + k - 2;
                if (j >= 0 && j < (long)env.size()) acc += w[k] * env[j];
            }
            sm[i] = acc;
        }
        double ms = 0;
        for (double v : sm) ms += v * v;
        double thr = 0.5 * sqrt(ms / (sm.empty() ? 1 : sm.size()));
        // Map back: suppress beats while localscore <= thr from both ends.
        std::vector<int> idx;
        for (int i = 0; i < n; i++)
            if (beats[i]) idx.push_back(i);
        size_t a = 0;
        while (a < idx.size() && ls[idx[a]] <= thr) {
            beats[idx[a]] = 0;
            a++;
        }
        long b = (long)idx.size() - 1;
        while (b >= (long)a && ls[idx[b]] <= thr) {
            beats[idx[b]] = 0;
            b--;
        }
    }
    for (int i = 0; i < n; i++)
        if (beats[i]) out.frames.push_back(i);
    return out;
}

// ── Chroma ───────────────────────────────────────────────────────────────────

static double hz_to_octs(double f, double tuning) {
    double a440 = 440.0 * pow(2.0, tuning / 12.0);
    return log2(f / (a440 / 16));
}

ChromaFB make_chroma(int sr, int n_fft, float tuning) {
    ChromaFB fb;
    fb.n_fft = n_fft;
    const int bins = stft_bins(n_fft), nc = 12;
    fb.w.assign((size_t)nc * bins, 0.f);
    std::vector<double> freqs((size_t)n_fft);
    for (int i = 0; i < n_fft; i++) freqs[i] = (double)i * sr / n_fft;
    // frqbins for bins 1..n_fft-1, then prepend bin0 - 1.5*12.
    std::vector<double> frq((size_t)n_fft);
    for (int i = 1; i < n_fft; i++) frq[i] = 12 * hz_to_octs(freqs[i], tuning);
    frq[0] = frq[1] - 18.0;
    std::vector<double> bw((size_t)n_fft);
    for (int i = 0; i < n_fft - 1; i++)
        bw[i] = std::max(frq[i + 1] - frq[i], 1.0);
    bw[n_fft - 1] = 1.0;
    const double n2 = 6.0;
    std::vector<std::vector<double>> w(nc, std::vector<double>((size_t)n_fft, 0.0));
    for (int c = 0; c < nc; c++) {
        for (int i = 0; i < n_fft; i++) {
            double d = fmod(frq[i] - c + n2 + 120.0, 12.0) - n2;
            w[c][i] = exp(-0.5 * pow(2 * d / bw[i], 2));
        }
    }
    // Normalise each column (l2), apply octave weighting, roll to base C.
    for (int i = 0; i < n_fft; i++) {
        double nrm = 0;
        for (int c = 0; c < nc; c++) nrm += w[c][i] * w[c][i];
        nrm = sqrt(nrm);
        if (nrm > 0)
            for (int c = 0; c < nc; c++) w[c][i] /= nrm;
        double oct = frq[i] / 12;
        double g = exp(-0.5 * pow((oct - 5.0) / 2.0, 2));
        for (int c = 0; c < nc; c++) w[c][i] *= g;
    }
    // base_c roll by -3.
    for (int c = 0; c < nc; c++) {
        int src = (c + 3) % 12;
        for (int b = 0; b < bins; b++) fb.w[(size_t)c * bins + b] = (float)w[src][b + 1];
    }
    return fb;
}

float estimate_tuning(const float* y, int n, int sr) {
    const int n_fft = 2048, hop = 512;
    auto win = hann_periodic(n_fft);
    std::vector<float> mag;
    int nfr = 0;
    stft_mag(y, n, n_fft, hop, true, win.data(), mag, nfr);
    if (nfr == 0) return 0.f;
    const int bins = stft_bins(n_fft);
    std::vector<float> S((size_t)bins * nfr);
    for (int i = 0; i < bins * nfr; i++) S[i] = mag[i];  // power=1 magnitude
    // Parabolic interpolation shifts along frequency.
    std::vector<float> shift((size_t)bins * nfr, 0.f);
    for (int t = 0; t < nfr; t++) {
        for (int f = 1; f < bins - 1; f++) {
            float xm = S[(size_t)(f - 1) * nfr + t], x0 = S[(size_t)f * nfr + t],
                  xp = S[(size_t)(f + 1) * nfr + t];
            double a = xp + xm - 2 * x0, b = (xp - xm) / 2;
            if (fabs(b) >= fabs(a)) continue;
            if (a == 0) continue;
            shift[(size_t)f * nfr + t] = (float)(-b / a);
        }
    }
    std::vector<double> grad((size_t)bins * nfr, 0.0);
    for (int t = 0; t < nfr; t++) {
        for (int f = 0; f < bins; f++) {
            int f0 = std::max(0, f - 1), f1 = std::min(bins - 1, f + 1);
            grad[(size_t)f * nfr + t] =
                (S[(size_t)f1 * nfr + t] - S[(size_t)f0 * nfr + t]) / (f1 - f0);
        }
    }
    const double fmin = 150.0, fmax = 4000.0;
    std::vector<double> res;
    res.reserve((size_t)bins * nfr / 8);
    for (int t = 0; t < nfr; t++) {
        float colmax = 0.f;
        for (int f = 0; f < bins; f++) colmax = fmaxf(colmax, S[(size_t)f * nfr + t]);
        float thr = 0.1f * colmax;
        for (int f = 0; f < bins; f++) {
            double ff = (double)f * sr / n_fft;
            if (!(ff >= fmin && ff < fmax)) continue;
            float v = S[(size_t)f * nfr + t];
            if (!(v > thr)) continue;
            bool lmax;
            if (f == 0)
                lmax = false;
            else if (f == bins - 1)
                lmax = v > S[(size_t)(f - 1) * nfr + t];
            else
                lmax = v > S[(size_t)(f - 1) * nfr + t] && v >= S[(size_t)(f + 1) * nfr + t];
            if (!lmax) continue;
            double pitch = (f + shift[(size_t)f * nfr + t]) * sr / n_fft;
            if (pitch > 0) {
                double r = fmod(12 * hz_to_octs(pitch, 0.0), 1.0);
                if (r < 0) r += 1.0;
                if (r >= 0.5) r -= 1.0;
                res.push_back(r);
            }
        }
    }
    if (res.empty()) return 0.f;
    const int nb = 101;
    std::vector<int> hist(nb, 0);
    for (double r : res) {
        int b = (int)floor((r + 0.5) * 100.0);
        if (b < 0) b = 0;
        if (b > 100) b = 100;
        hist[b]++;
    }
    int bi = (int)(std::max_element(hist.begin(), hist.end()) - hist.begin());
    return (float)(-0.5 + bi / 100.0);
}

void chroma_from_power(const ChromaFB& fb, const float* mag, int nfr,
                       std::vector<float>& c) {
    const int bins = stft_bins(fb.n_fft), nc = 12;
    c.assign((size_t)nc * nfr, 0.f);
    for (int t = 0; t < nfr; t++) {
        for (int i = 0; i < nc; i++) {
            const float* wrow = fb.w.data() + (size_t)i * bins;
            double acc = 0.0;
            for (int b = 0; b < bins; b++) {
                float v = mag[(size_t)b * nfr + t];
                acc += (double)wrow[b] * v * v;
            }
            c[(size_t)i * nfr + t] = (float)acc;
        }
        float mx = 0.f;
        for (int i = 0; i < nc; i++) mx = fmaxf(mx, fabsf(c[(size_t)i * nfr + t]));
        if (mx > 0)
            for (int i = 0; i < nc; i++) c[(size_t)i * nfr + t] /= mx;
    }
}

std::vector<float> sync_median(const float* c, int rows, int nfr, const int* bounds,
                               int nb) {
    std::vector<float> out((size_t)rows * nb, 0.f);
    std::vector<float> tmp;
    for (int i = 0; i < nb; i++) {
        int a = bounds[i], b = (i + 1 <= nb) ? bounds[i + 1] : nfr;
        if (a < 0) a = 0;
        if (b > nfr) b = nfr;
        for (int r = 0; r < rows; r++) {
            if (b <= a) {
                out[(size_t)r * nb + i] = 0.f;
                continue;
            }
            tmp.clear();
            for (int t = a; t < b; t++) tmp.push_back(c[(size_t)r * nfr + t]);
            out[(size_t)r * nb + i] = percentile_sorted(std::move(tmp), 0.5f);
        }
    }
    return out;
}

int downbeat_phase(const float* bo, int n, const int* bf, int nbeats,
                   const float* chroma, int nfr) {
    if (nbeats <= 0) return 0;
    float bomax = 1e-9f;
    for (int i = 0; i < n; i++) bomax = fmaxf(bomax, bo[i]);
    // bounds = beat frames + nfr sentinel; sync needs nb+1 entries.
    std::vector<int> bounds((size_t)nbeats + 1);
    for (int i = 0; i < nbeats; i++) bounds[i] = bf[i];
    bounds[nbeats] = nfr;
    auto sync = sync_median(chroma, 12, nfr, bounds.data(), nbeats);
    // Column l2-normalise (librosa chroma_stft already inf-normalises per
    // column; the spec's extra normalisation is l2 over the 12 bins).
    for (int i = 0; i < nbeats; i++) {
        double s = 0;
        for (int r = 0; r < 12; r++) {
            double v = sync[(size_t)r * nbeats + i];
            s += v * v;
        }
        s = sqrt(s) + 1e-9;
        for (int r = 0; r < 12; r++) sync[(size_t)r * nbeats + i] /= (float)s;
    }
    std::vector<double> nov((size_t)nbeats, 0.0);
    for (int i = 1; i < nbeats; i++) {
        double d = 0;
        for (int r = 0; r < 12; r++)
            d += sync[(size_t)r * nbeats + i] * sync[(size_t)r * nbeats + i - 1];
        nov[i] = 1.0 - d;
    }
    double score[4] = {0, 0, 0, 0};
    for (int i = 0; i < nbeats; i++) {
        int b = bf[i];
        float m = 0.f;
        for (int k = std::max(0, b - 3); k <= std::min(n - 1, b + 3); k++)
            m = fmaxf(m, bo[k]);
        score[i % 4] += m / bomax + nov[i];
    }
    int best = 0;
    for (int i = 1; i < 4; i++)
        if (score[i] > score[best]) best = i;
    return best;
}

std::vector<float> frame_rms(const float* y, int n, int frame_len, int hop,
                             int n_frames) {
    std::vector<float> out((size_t)n_frames, 0.f);
    for (int t = 0; t < n_frames; t++) {
        int s = t * hop;
        double acc = 0.0;
        for (int i = 0; i < frame_len; i++) {
            float v = (s + i < n) ? y[s + i] : 0.f;
            acc += (double)v * v;
        }
        out[t] = (float)sqrt(acc / frame_len);
    }
    return out;
}

}  // namespace aadsp
