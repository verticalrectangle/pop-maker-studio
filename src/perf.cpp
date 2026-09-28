#include "perf.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

namespace perf {
namespace {

std::array<StageStats, S_COUNT> g_stages{};
std::mutex g_stage_mu;

// Async CPU decode accumulator (workers add, UI thread drains per frame).
std::atomic<double> g_decode_accum{0.0};
std::atomic<uint64_t> g_decode_n{0};

// UI frame-time ring: rolling window of recent presented frames.
constexpr size_t kFrameRing = 600;
std::vector<double> g_frames;
size_t g_frame_head = 0;
std::mutex g_frame_mu;

double now_ms() {
    using clock = std::chrono::steady_clock;
    static const auto t0 = clock::now();
    return std::chrono::duration<double, std::milli>(clock::now() - t0).count();
}

}  // namespace

void record(Stage s, double ms) {
    if (s < 0 || s >= S_COUNT) return;
    std::lock_guard<std::mutex> lk(g_stage_mu);
    StageStats& st = g_stages[(size_t)s];
    st.last = ms;
    st.n++;
    const double a = 0.1;
    st.ema = (st.n == 1) ? ms : st.ema + a * (ms - st.ema);
    if (ms > st.max) st.max = ms;
}

void record_decode_cpu(double ms) {
    // atomic<double> fetch_add via CAS loop (C++17 has no fetch_add for double).
    double cur = g_decode_accum.load(std::memory_order_relaxed);
    while (!g_decode_accum.compare_exchange_weak(cur, cur + ms,
                                                 std::memory_order_relaxed))
        ;
    g_decode_n.fetch_add(1, std::memory_order_relaxed);
}

double drain_decode() {
    uint64_t n = g_decode_n.exchange(0, std::memory_order_relaxed);
    if (n == 0) return 0.0;
    double acc = g_decode_accum.exchange(0.0, std::memory_order_relaxed);
    double mean = acc / (double)n;
    record(S_DECODE, mean);
    (void)now_ms;
    return mean;
}

void frame_sample(double dt_ms) {
    std::lock_guard<std::mutex> lk(g_frame_mu);
    if (g_frames.size() < kFrameRing) {
        g_frames.push_back(dt_ms);
        return;
    }
    g_frames[g_frame_head] = dt_ms;
    g_frame_head = (g_frame_head + 1) % kFrameRing;
}

void frame_percentiles(double& p50, double& p95, double& p99, double& maxv) {
    std::vector<double> v;
    {
        std::lock_guard<std::mutex> lk(g_frame_mu);
        v = g_frames;
    }
    if (v.empty()) { p50 = p95 = p99 = maxv = 0.0; return; }
    std::sort(v.begin(), v.end());
    auto q = [&](double p) { return v[std::min(v.size() - 1, (size_t)(p * v.size()))]; };
    p50 = q(0.50); p95 = q(0.95); p99 = q(0.99); maxv = v.back();
}

void frame_reset() {
    std::lock_guard<std::mutex> lk(g_frame_mu);
    g_frames.clear();
    g_frame_head = 0;
}

void stage_stats(Stage s, StageStats& out) {
    if (s < 0 || s >= S_COUNT) { out = {}; return; }
    std::lock_guard<std::mutex> lk(g_stage_mu);
    out = g_stages[(size_t)s];
}

const char* stage_name(Stage s) {
    switch (s) {
        case S_PREFETCH: return "prefetch";
        case S_DECODE:   return "decode";
        case S_UPLOAD:   return "upload";
        case S_CLIPFX:   return "clip_fx";
        case S_COMPOSITE:return "composite";
        case S_TEXT:     return "text";
        case S_SHAPES:   return "shapes";
        case S_SWAP:     return "swap";
        default:         return "?";
    }
}

}  // namespace perf
