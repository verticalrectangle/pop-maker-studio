#include "av_measure.h"
#include "av_sync.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

std::thread            s_worker;
std::atomic<bool>      s_active{false};   // capture/analysis running
std::atomic<bool>      s_done{false};     // result ready to consume
std::atomic<long long> s_start_ms{0};     // wall-clock start (epoch ms)
std::atomic<float>     s_capture_s{0.f};
std::mutex             s_mtx;
AVMeasureResult        s_result;          // guarded by s_mtx

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

// Minimal RIFF/WAVE reader for the exact format we write below: PCM mono
// 16-bit little-endian. Walks the chunk list to the "data" chunk (ffmpeg can
// emit a LIST/INFO chunk before it), then maps int16 → float in [-1,1].
bool read_pcm16_mono(const std::string& path, std::vector<float>& out, int& sr) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    auto rd32 = [&](uint32_t& v) { return fread(&v, 4, 1, f) == 1; };
    auto rd16 = [&](uint16_t& v) { return fread(&v, 2, 1, f) == 1; };
    char riff[4], wave[4];
    uint32_t sz = 0;
    if (fread(riff, 1, 4, f) != 4 || memcmp(riff, "RIFF", 4) != 0) { fclose(f); return false; }
    if (!rd32(sz) || fread(wave, 1, 4, f) != 4 || memcmp(wave, "WAVE", 4) != 0) { fclose(f); return false; }

    uint16_t channels = 0, bits = 0;
    uint32_t rate = 0;
    bool have_fmt = false;
    for (;;) {
        char id[4]; uint32_t csz = 0;
        if (fread(id, 1, 4, f) != 4 || !rd32(csz)) { fclose(f); return false; }
        if (memcmp(id, "fmt ", 4) == 0) {
            uint16_t fmt = 0, balign = 0; uint32_t byterate = 0;
            if (!rd16(fmt) || !rd16(channels) || !rd32(rate) ||
                !rd32(byterate) || !rd16(balign) || !rd16(bits)) { fclose(f); return false; }
            // skip any extra fmt bytes
            if (csz > 16) fseek(f, (long)(csz - 16), SEEK_CUR);
            have_fmt = true;
        } else if (memcmp(id, "data", 4) == 0) {
            if (!have_fmt || bits != 16 || channels < 1) { fclose(f); return false; }
            size_t nsamp = csz / 2;                 // total int16 samples (all channels)
            std::vector<int16_t> pcm(nsamp);
            size_t got = fread(pcm.data(), 2, nsamp, f);
            fclose(f);
            out.clear();
            out.reserve(got / channels + 1);
            // Downmix to mono (we already ask ffmpeg for -ac 1, but be safe).
            for (size_t i = 0; i + channels <= got; i += channels) {
                int acc = 0;
                for (int c = 0; c < channels; ++c) acc += pcm[i + (size_t)c];
                out.push_back((float)acc / (float)channels / 32768.f);
            }
            sr = (int)rate;
            return !out.empty();
        } else {
            fseek(f, (long)csz + (csz & 1), SEEK_CUR);  // chunks are word-aligned
        }
    }
}

void run_measure(std::string clean_src, std::string cam_src, float capture_s) {
    AVMeasureResult res;

    long long stamp = now_ms();
    std::string base = "/tmp/pms_avmeasure_" + std::to_string(stamp);
    std::string clean_wav = base + "_clean.wav";
    std::string cam_wav   = base + "_cam.wav";

    // One ffmpeg, two pulse inputs, two WAV outputs → shared start instant.
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -hide_banner -loglevel error "
             "-f pulse -i '%s' -f pulse -i '%s' "
             "-map 0:a -c:a pcm_s16le -ar 44100 -ac 1 -t %.2f -y '%s' "
             "-map 1:a -c:a pcm_s16le -ar 44100 -ac 1 -t %.2f -y '%s' 2>/dev/null",
             clean_src.c_str(), cam_src.c_str(),
             capture_s, clean_wav.c_str(),
             capture_s, cam_wav.c_str());

    int rc = system(cmd);
    if (rc != 0) {
        res.error = "Couldn't record the two mics (is the camera mic free?).";
    } else {
        std::vector<float> a, b;
        int sra = 0, srb = 0;
        if (!read_pcm16_mono(clean_wav, a, sra) ||
            !read_pcm16_mono(cam_wav, b, srb)) {
            res.error = "Recorded audio was empty or unreadable.";
        } else if (sra != srb) {
            res.error = "Mic sample rates didn't match.";
        } else {
            float conf = 0.f;
            // a = clean mic (kept), b = camera scratch mic (≈ video timing).
            // GCC-PHAT positive ⇒ b lags a ⇒ the video event lands later than the
            // clean audio ⇒ audio currently leads ⇒ delay the audio by that much.
            // rec_av_offset_ms is "+ delays the audio", so the sign maps straight
            // through. (Worth a real-clap sanity check on first hardware run.)
            float lag = av_estimate_offset_seconds(a.data(), a.size(),
                                                   b.data(), b.size(),
                                                   sra, 0.5f, &conf);
            res.ok         = true;
            res.offset_ms  = lag * 1000.f;
            res.confidence = conf;
        }
    }

    remove(clean_wav.c_str());
    remove(cam_wav.c_str());

    {
        std::lock_guard<std::mutex> lk(s_mtx);
        s_result = res;
    }
    s_active.store(false);
    s_done.store(true);
}

} // namespace

bool av_measure_start(const std::string& clean_pulse_src,
                      const std::string& cam_pulse_src,
                      float capture_s) {
    if (s_active.load()) return false;
    if (clean_pulse_src.empty() || cam_pulse_src.empty()) return false;
    if (capture_s < 0.5f) capture_s = 0.5f;

    if (s_worker.joinable()) s_worker.join();   // reap the previous run
    s_done.store(false);
    s_active.store(true);
    s_start_ms.store(now_ms());
    s_capture_s.store(capture_s);
    s_worker = std::thread(run_measure, clean_pulse_src, cam_pulse_src, capture_s);
    return true;
}

bool av_measure_active() { return s_active.load(); }

bool av_measure_poll(AVMeasureResult& out) {
    if (!s_done.exchange(false)) return false;
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        out = s_result;
    }
    if (s_worker.joinable()) s_worker.join();
    return true;
}

float av_measure_elapsed() {
    if (!s_active.load()) return 0.f;
    float e = (float)(now_ms() - s_start_ms.load()) / 1000.f;
    return e < 0.f ? 0.f : e;
}

void av_measure_shutdown() {
    if (s_worker.joinable()) s_worker.join();
}
