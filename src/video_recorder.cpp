#include "video_recorder.h"
#include "audio.h"
#include "face_cache.h"
#include "history.h"
#include "globals.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

// Camera→screen→performer latency: frames are captured this long before the
// loop position they correspond to. Webcams typically run 2–4 frames behind;
// 100 ms is a sane default until per-device calibration exists.
static constexpr float kCamLatency = 0.100f;

struct VFrame {
    float                t = 0.f;   // loop-stream time (seconds since loop start 0)
    std::vector<uint8_t> jpeg;
};

enum class VRecState { Idle, Monitor, Warming, Recording };

static VRecState s_state = VRecState::Idle;
static bool   s_monitor_want = false;     // user asked for the live mirror
static bool   s_test_pattern = false;
static int    s_device_sel = -1;          // index into vrecorder_devices(); -1 auto
static int    s_ti = -1, s_ci = -1;
static float  s_lp_start = 0.f, s_lp_end = 0.f;
static float  s_loop_len = 0.f;
static int    s_take_count = 0;
static FILE*  s_cap = nullptr;            // capture child stdout (MJPEG stream)
static pid_t  s_cap_pid = 0;              // capture child pid (own process group)
static std::vector<uint8_t> s_raw;        // undecoded byte tail from the pipe
static std::vector<VFrame>  s_frames;     // frames of the take in progress
static std::vector<uint8_t> s_last_jpeg;  // most recent frame (live mirror)
static uint64_t s_frame_serial = 0;
static std::string s_error;
static std::chrono::steady_clock::time_point s_t0;       // capture stream origin
static std::chrono::steady_clock::time_point s_warm_t0;  // warmup start

// ── Devices ───────────────────────────────────────────────────────────────────

std::vector<VCamDevice> vrecorder_devices() {
    std::vector<VCamDevice> out;
    for (int i = 0; i < 16; ++i) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/video%d", i);
        int fd = open(path, O_RDWR | O_NONBLOCK);
        if (fd < 0) continue;
        v4l2_capability cap{};
        bool is_capture = false;
        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
            uint32_t c = (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
                         ? cap.device_caps : cap.capabilities;
            is_capture = (c & V4L2_CAP_VIDEO_CAPTURE) != 0;
        }
        if (is_capture)
            out.push_back({path, (const char*)cap.card});
        close(fd);
    }
    return out;
}

void vrecorder_device_select(int idx) { s_device_sel = idx; }
int  vrecorder_device_selected()      { return s_device_sel; }

// Does the device deliver MJPEG natively? (Stream-copy when it does; phone
// bridges like v4l2loopback often only do planar YUV — re-encode those.)
static bool device_has_mjpeg(const std::string& path) {
    // Cache per device path: the format enumeration opens the V4L2 node, which
    // on some bridges blocks ~100 ms — re-running it on every capture_start
    // slowed the warm-up after the camera is released between takes.
    static std::map<std::string, bool> s_mjpeg_cache;
    auto it = s_mjpeg_cache.find(path);
    if (it != s_mjpeg_cache.end()) return it->second;
    int fd = open(path.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) return false;            // don't cache a transient open failure
    bool found = false;
    for (uint32_t i = 0; i < 64; ++i) {
        v4l2_fmtdesc f{};
        f.index = i;
        f.type  = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd, VIDIOC_ENUM_FMT, &f) != 0) break;
        if (f.pixelformat == V4L2_PIX_FMT_MJPEG) { found = true; break; }
    }
    close(fd);
    s_mjpeg_cache[path] = found;
    return found;
}

// ── Helpers ───────────────────────────────────────────────────────────────────

static Clip* target_brick(AppState& state) {
    auto match = [&](int ti, int ci) -> Clip* {
        if (ti < 0 || ti >= (int)state.tracks.size()) return nullptr;
        auto& clips = state.tracks[ti].clips;
        if (ci < 0 || ci >= (int)clips.size()) return nullptr;
        Clip* cl = &clips[ci];
        if (cl->clip_type != ClipType::VideoRecord) return nullptr;
        bool rec = s_state == VRecState::Warming || s_state == VRecState::Recording;
        if (rec && (fabsf(cl->start - s_lp_start) > 1e-4f ||
                    fabsf(cl->end   - s_lp_end)   > 1e-4f)) return nullptr;
        return cl;
    };
    if (Clip* cl = match(s_ti, s_ci)) return cl;
    if (s_state != VRecState::Warming && s_state != VRecState::Recording)
        return nullptr;
    for (int ti = 0; ti < (int)state.tracks.size(); ++ti)
        for (int ci = 0; ci < (int)state.tracks[ti].clips.size(); ++ci)
            if (Clip* cl = match(ti, ci)) { s_ti = ti; s_ci = ci; return cl; }
    return nullptr;
}

static std::string takes_dir() {
    std::string dir = g_managed_dir + "/takes";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

static std::string next_take_path() {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    long ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    char buf[160];
    snprintf(buf, sizeof(buf), "%s/vtake_%ld_%02d.avi",
             takes_dir().c_str(), ms, s_take_count + 1);
    return buf;
}

// JPEG dimensions from the SOF0/SOF2 marker (needed for the AVI headers).
static bool jpeg_dims(const std::vector<uint8_t>& j, int& w, int& h) {
    for (size_t i = 2; i + 9 < j.size(); ) {
        if (j[i] != 0xFF) { ++i; continue; }
        uint8_t m = j[i+1];
        if (m == 0xC0 || m == 0xC1 || m == 0xC2) {
            h = (j[i+5] << 8) | j[i+6];
            w = (j[i+7] << 8) | j[i+8];
            return w > 0 && h > 0;
        }
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD9)) { i += 2; continue; }
        i += 2 + ((j[i+2] << 8) | j[i+3]);   // skip marker segment
    }
    return false;
}

// Hand-rolled MJPEG-in-AVI muxer. ffmpeg's mjpeg demuxer mishandles
// non-integer -framerate values (measured: 32.25 fps in produces a file a
// third of the right length), and the AVI stream header's scale/rate pair
// holds our exact rational anyway: rate = nframes·1000, scale = loop_ms.
static bool write_take_avi(const std::string& path,
                            const std::vector<VFrame>& frames, float dur_s) {
    int w = 0, h = 0;
    if (!jpeg_dims(frames[0].jpeg, w, h)) return false;
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;

    uint32_t n = (uint32_t)frames.size();
    uint32_t rate  = n * 1000u;
    uint32_t scale = (uint32_t)lroundf(dur_s * 1000.f);
    uint32_t usec_pf = (uint32_t)llroundf(dur_s * 1e6f / (float)n);

    auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    auto tag = [&](const char* t) { fwrite(t, 4, 1, f); };

    uint32_t movi_body = 0;                  // sum of '00dc' chunks (padded)
    std::vector<uint32_t> sizes(n);
    for (uint32_t i = 0; i < n; ++i) {
        sizes[i] = (uint32_t)frames[i].jpeg.size();
        movi_body += 8 + sizes[i] + (sizes[i] & 1);
    }
    uint32_t hdrl_sz = 4 + (8+56) + (8 + 4 + (8+56) + (8+40));
    uint32_t movi_sz = 4 + movi_body;
    uint32_t idx_sz  = n * 16;
    uint32_t riff_sz = 4 + (8 + hdrl_sz) + (8 + movi_sz) + (8 + idx_sz);

    tag("RIFF"); u32(riff_sz); tag("AVI ");
    tag("LIST"); u32(hdrl_sz); tag("hdrl");
    tag("avih"); u32(56);
    u32(usec_pf); u32(0); u32(0); u32(0x10 /*HASINDEX*/);
    u32(n); u32(0); u32(1); u32(0);
    u32((uint32_t)w); u32((uint32_t)h);
    u32(0); u32(0); u32(0); u32(0);
    tag("LIST"); u32(4 + (8+56) + (8+40)); tag("strl");
    tag("strh"); u32(56);
    tag("vids"); tag("MJPG");
    u32(0); u16(0); u16(0); u32(0);
    u32(scale); u32(rate);
    u32(0); u32(n); u32(0); u32((uint32_t)-1); u32(0);
    u16(0); u16(0); u16((uint16_t)w); u16((uint16_t)h);
    tag("strf"); u32(40);
    u32(40); u32((uint32_t)w); u32((uint32_t)h);
    u16(1); u16(24); tag("MJPG");
    u32((uint32_t)(w * h * 3)); u32(0); u32(0); u32(0); u32(0);
    tag("LIST"); u32(movi_sz); tag("movi");
    std::vector<uint32_t> offs(n);
    uint32_t off = 4;                        // relative to 'movi' tag start
    for (uint32_t i = 0; i < n; ++i) {
        offs[i] = off;
        tag("00dc"); u32(sizes[i]);
        fwrite(frames[i].jpeg.data(), 1, sizes[i], f);
        if (sizes[i] & 1) fputc(0, f);
        off += 8 + sizes[i] + (sizes[i] & 1);
    }
    tag("idx1"); u32(idx_sz);
    for (uint32_t i = 0; i < n; ++i) {
        tag("00dc"); u32(0x10 /*KEYFRAME*/); u32(offs[i]); u32(sizes[i]);
    }
    bool ok = !ferror(f);
    fclose(f);
    if (!ok) ::unlink(path.c_str());
    return ok;
}

// Write the take with duration == loop length exactly.
static bool finalize_take(AppState& state) {
    if (s_frames.size() < 2 || s_loop_len <= 0.f) { s_frames.clear(); return true; }
    std::string path = next_take_path();
    bool ok = write_take_avi(path, s_frames, s_loop_len);
    s_frames.clear();
    if (!ok) { ::unlink(path.c_str()); return false; }

    Clip* cl = target_brick(state);
    if (!cl) { ::unlink(path.c_str()); return false; }
    cl->rec_takes.push_back(path);
    cl->rec_take_sel = (int)cl->rec_takes.size() - 1;
    cl->text = path;   // mirrored: video draw/export paths read clip.text
    state.proxy_scan_needed = true;   // proxy + slot for canvas preview
    ++s_take_count;
    // Face filter active on the brick → start the take's landmark pass now,
    // so playback is already filtered by the time the user hits play.
    if (cl->face_filter != 0) {
        int rq = ((int)lroundf(cl->rotation / 90.f) % 4 + 4) % 4;
        face_cache_request(path, rq);
    }
    return true;
}

// ── Capture child ─────────────────────────────────────────────────────────────

static bool capture_start() {
    auto devs = vrecorder_devices();
    s_test_pattern = devs.empty();
    char cmd[512];
    // Deterministic test rig: PMS_FAKE_CAM=<image> loops a still through the
    // REAL capture path — standard reference faces instead of a live human
    // when validating face tracking/filters.
    if (const char* fake = getenv("PMS_FAKE_CAM")) {
        // Stills loop at 15 fps; video files (motion test clips) loop whole.
        const char* ext = strrchr(fake, '.');
        bool is_video = ext && (!strcasecmp(ext, ".mp4") || !strcasecmp(ext, ".avi") ||
                                !strcasecmp(ext, ".mkv") || !strcasecmp(ext, ".mov") ||
                                !strcasecmp(ext, ".webm"));
        snprintf(cmd, sizeof(cmd),
                 "ffmpeg -hide_banner -loglevel error"
                 " -re %s -i '%s'"
                 " -vf 'scale=720:1280:force_original_aspect_ratio=decrease,"
                 "pad=720:1280:(ow-iw)/2:(oh-ih)/2'"
                 " -an -c:v mjpeg -q:v 4 -f mjpeg pipe:1 2>/dev/null",
                 is_video ? "-stream_loop -1" : "-loop 1 -framerate 15", fake);
    } else if (s_test_pattern) {
        // Camera-less dev fallback: timestamped test pattern at webcam-ish
        // specs. -re paces lavfi to real time (a camera self-paces).
        snprintf(cmd, sizeof(cmd),
                 "ffmpeg -hide_banner -loglevel error"
                 " -re -f lavfi -i testsrc2=size=1280x720:rate=30"
                 " -c:v mjpeg -q:v 4 -f mjpeg pipe:1 2>/dev/null");
    } else {
        int idx = (s_device_sel >= 0 && s_device_sel < (int)devs.size())
                  ? s_device_sel : 0;
        const std::string& dev = devs[(size_t)idx].path;
        // Low-latency intake: skip ffmpeg's stream analysis so first frame
        // arrives ~immediately — the camera is released between takes, so a
        // short cold start matters. nobuffer/low_delay + a tiny probe window.
        const char* lat = "-fflags nobuffer -flags low_delay "
                          "-probesize 32 -analyzeduration 0";
        if (device_has_mjpeg(dev)) {
            // Native MJPEG → stream copy, no transcode.
            snprintf(cmd, sizeof(cmd),
                     "ffmpeg -hide_banner -loglevel error %s"
                     " -f v4l2 -input_format mjpeg -framerate 30"
                     " -video_size 1280x720 -i %s"
                     " -c:v copy -f mjpeg pipe:1 2>/dev/null", lat, dev.c_str());
        } else {
            // YUV-only device (e.g. phone bridges over v4l2loopback):
            // let V4L2 negotiate format/size and encode to MJPEG ourselves.
            snprintf(cmd, sizeof(cmd),
                     "ffmpeg -hide_banner -loglevel error %s"
                     " -f v4l2 -i %s"
                     " -c:v mjpeg -q:v 4 -f mjpeg pipe:1 2>/dev/null",
                     lat, dev.c_str());
        }
    }
    // Own fork/exec instead of popen: pclose() WAITS for the child, and
    // ffmpeg sometimes wedges in a futex on teardown instead of dying on the
    // closed pipe — pclose then blocks the MAIN THREAD forever (frozen app,
    // dead IPC). With the pid in hand we can kill the process group and wait
    // with a bound.
    int fds[2];
    if (pipe(fds) != 0) { s_error = "pipe() failed"; return false; }
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]); close(fds[1]);
        s_error = "fork() failed";
        return false;
    }
    if (pid == 0) {
        setpgid(0, 0);              // own group → killable with children
        dup2(fds[1], 1);
        close(fds[0]); close(fds[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char*)nullptr);
        _exit(127);
    }
    close(fds[1]);
    s_cap = fdopen(fds[0], "r");
    if (!s_cap) {
        close(fds[0]);
        kill(-pid, SIGKILL);
        waitpid(pid, nullptr, 0);
        s_error = "fdopen() failed";
        return false;
    }
    s_cap_pid = pid;
    int fd = fileno(s_cap);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return true;
}

static void capture_stop() {
    if (s_cap) {
        fclose(s_cap);              // reader side gone → child gets EPIPE
        s_cap = nullptr;
        if (s_cap_pid > 0) {
            kill(-s_cap_pid, SIGTERM);
            // Bounded reap: ~1 s of WNOHANG polls, then SIGKILL. Never block
            // the main thread on ffmpeg teardown again.
            for (int i = 0; i < 20; ++i) {
                if (waitpid(s_cap_pid, nullptr, WNOHANG) != 0) { s_cap_pid = 0; break; }
                usleep(50 * 1000);
            }
            if (s_cap_pid > 0) {
                kill(-s_cap_pid, SIGKILL);
                waitpid(s_cap_pid, nullptr, 0);
                s_cap_pid = 0;
            }
        }
    }
    s_raw.clear();
    s_raw.shrink_to_fit();
}

// App-shutdown hook: force-kill the capture child so closing PMS (even with a
// CAM track / camera preview live) never orphans its ffmpeg. Idempotent.
void vrecorder_shutdown() {
    capture_stop();
}

// Drain the pipe and split the MJPEG byte stream into frames on JPEG
// SOI (FFD8) / EOI (FFD9) markers. Each completed frame is stamped with the
// current loop-stream time.
static void capture_drain(bool keep_frames) {
    if (!s_cap) return;
    uint8_t buf[1 << 16];
    ssize_t n;
    int fd = fileno(s_cap);
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        s_raw.insert(s_raw.end(), buf, buf + n);

    float now_t = std::chrono::duration<float>(
                      std::chrono::steady_clock::now() - s_t0).count();

    size_t pos = 0;
    while (true) {
        size_t soi = pos;
        while (soi + 1 < s_raw.size() &&
               !(s_raw[soi] == 0xFF && s_raw[soi+1] == 0xD8)) ++soi;
        if (soi + 1 >= s_raw.size()) break;
        size_t eoi = soi + 2;
        while (eoi + 1 < s_raw.size() &&
               !(s_raw[eoi] == 0xFF && s_raw[eoi+1] == 0xD9)) ++eoi;
        if (eoi + 1 >= s_raw.size()) break;

        s_last_jpeg.assign(s_raw.begin() + (long)soi, s_raw.begin() + (long)eoi + 2);
        ++s_frame_serial;
        if (keep_frames) {
            VFrame f;
            f.t = now_t - kCamLatency;
            f.jpeg = s_last_jpeg;
            s_frames.push_back(std::move(f));
        }
        pos = eoi + 2;
    }
    if (pos > 0) s_raw.erase(s_raw.begin(), s_raw.begin() + (long)pos);
}

// ── Public API ────────────────────────────────────────────────────────────────

bool vrecorder_active()    { return s_state == VRecState::Warming ||
                                    s_state == VRecState::Recording; }
bool vrecorder_recording() { return s_state == VRecState::Recording; }
bool vrecorder_warming()   { return s_state == VRecState::Warming; }
bool vrecorder_is_target(int ti, int ci) {
    return vrecorder_active() && ti == s_ti && ci == s_ci;
}
int  vrecorder_take_count() { return s_take_count; }
bool vrecorder_using_test_pattern() { return s_cap && s_test_pattern; }
std::string vrecorder_error() { return s_error; }
uint64_t vrecorder_frame_serial() { return s_frame_serial; }

bool vrecorder_latest_jpeg(std::vector<uint8_t>& out) {
    if (s_last_jpeg.empty()) return false;
    out = s_last_jpeg;
    return true;
}

void vrecorder_monitor_set(bool on) {
    s_monitor_want = on;
    if (on && s_state == VRecState::Idle) {
        s_error.clear();
        if (capture_start()) {
            s_state = VRecState::Monitor;
            s_t0 = std::chrono::steady_clock::now();
        }
    } else if (!on && s_state == VRecState::Monitor) {
        capture_stop();
        s_last_jpeg.clear();
        s_state = VRecState::Idle;
    }
}
bool vrecorder_monitor_get() { return s_monitor_want; }

bool vrecorder_start(AppState& state, int ti, int ci) {
    if (vrecorder_active()) return false;
    s_error.clear();
    s_ti = ti; s_ci = ci;
    s_state = VRecState::Idle;   // so target_brick skips the bounds check
    Clip* cl = target_brick(state);
    if (!cl || cl->end - cl->start < 0.25f) {
        s_ti = s_ci = -1;
        s_error = "brick too short to loop (needs at least 0.25 s)";
        s_state = s_monitor_want && s_cap ? VRecState::Monitor : VRecState::Idle;
        return false;
    }
    s_lp_start = cl->start;
    s_lp_end   = cl->end;
    s_loop_len = s_lp_end - s_lp_start;

    // Reuse the monitor's capture child when it's already running.
    if (!s_cap && !capture_start()) {
        s_ti = s_ci = -1;
        s_state = VRecState::Idle;
        return false;
    }

    // Async warmup: the tick starts the transport when the first frame
    // arrives (or errors out) — the UI never blocks.
    s_frames.clear();
    s_take_count = 0;
    s_warm_t0 = std::chrono::steady_clock::now();
    s_state = VRecState::Warming;
    return true;
}

static void begin_transport(AppState& state) {
    audio_init();
    bool join = state.playing && audio_loop_active();
    if (!join) {
        audio_set_loop(s_lp_start, s_lp_end);
        audio_seek(s_lp_start);
        state.playhead        = s_lp_start;
        state.playing         = true;
        state.play_start_pos  = s_lp_start;
        state.play_start_wall = std::chrono::steady_clock::now();
        audio_play();
        s_t0 = std::chrono::steady_clock::now();
    } else {
        float cur = fmodf(fmaxf(0.f,
                        std::chrono::duration<float>(
                            std::chrono::steady_clock::now()
                            - state.play_start_wall).count()
                        + state.play_start_pos - s_lp_start),
                    fmaxf(s_loop_len, 1e-3f));
        s_t0 = std::chrono::steady_clock::now() -
               std::chrono::microseconds((long)(cur * 1e6f));
    }
    s_frames.clear();
    s_state = VRecState::Recording;
}

void vrecorder_stop(AppState& state, bool keep_partial) {
    if (!vrecorder_active()) return;
    bool was_recording = s_state == VRecState::Recording;
    if (was_recording) capture_drain(true);

    // Keep a partial last pass when it has at least half a second in it.
    if (was_recording && keep_partial && !s_frames.empty()) {
        float span = s_frames.back().t - s_frames.front().t;
        if (span > 0.5f) {
            float keep_len = s_loop_len;
            s_loop_len = fmaxf(span, 0.5f);
            finalize_take(state);
            s_loop_len = keep_len;
        }
    }
    s_frames.clear();

    if (s_take_count > 0)
        history_push(state, "Record " + std::to_string(s_take_count) +
                            " video take(s)");

    // Only tear the transport down if the audio recorder isn't still using it.
    extern bool recorder_active();
    if (was_recording && !recorder_active()) {
        audio_clear_loop();
        state.playing = false;
        audio_pause();
        // Park the playhead at the take's start so the freshly recorded take
        // is shown from its first frame (instead of wherever the loop ended).
        state.playhead = s_lp_start;
    }

    s_ti = s_ci = -1;
    // A finished recording always drops the live preview and releases the
    // camera — you want to see the TAKE you just shot, not the live feed
    // painted over it, and an idle camera proc should close. Re-arming Record
    // re-opens the device and shows live again (vrecorder_start → Warming).
    s_monitor_want = false;
    capture_stop();
    s_last_jpeg.clear();
    s_state = VRecState::Idle;
}

void vrecorder_tick(AppState& state) {
    if (s_state == VRecState::Monitor) {
        capture_drain(false);
        // Child died (device unplugged)? Surface it and stop.
        if (s_cap && feof(s_cap)) {
            capture_stop();
            s_last_jpeg.clear();
            s_error = "camera stream ended";
            s_state = VRecState::Idle;
        }
        return;
    }
    if (s_state == VRecState::Warming) {
        if (!target_brick(state)) { vrecorder_stop(state, false); return; }
        uint64_t before = s_frame_serial;
        capture_drain(false);
        if (s_frame_serial != before) {
            begin_transport(state);   // first frame arrived — go
            return;
        }
        float waited = std::chrono::duration<float>(
                           std::chrono::steady_clock::now() - s_warm_t0).count();
        if (waited > 6.f) {
            s_error = "camera produced no frames — check the device picker "
                      "(is the camera app/stream running?)";
            capture_stop();
            s_last_jpeg.clear();
            s_ti = s_ci = -1;
            s_state = VRecState::Idle;
        }
        return;
    }
    if (s_state != VRecState::Recording) return;

    if (!target_brick(state) || !state.playing || !audio_loop_active()) {
        bool keep = target_brick(state) != nullptr;
        vrecorder_stop(state, keep);
        return;
    }

    capture_drain(true);

    // Every frame past (take_count+1)·loop_len closes a take. Frames are
    // bucketed by their stamped loop-stream time, so a slow camera can't
    // smear takes across boundaries.
    while (!s_frames.empty()) {
        float boundary = (float)(s_take_count + 1) * s_loop_len;
        size_t split = 0;
        while (split < s_frames.size() && s_frames[split].t < boundary) ++split;
        if (split >= s_frames.size()) break;   // boundary not reached yet

        std::vector<VFrame> rest(s_frames.begin() + (long)split, s_frames.end());
        s_frames.resize(split);
        if (!finalize_take(state)) {
            vrecorder_stop(state, false);
            return;
        }
        s_frames = std::move(rest);
    }
}
