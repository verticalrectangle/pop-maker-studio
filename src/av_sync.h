#pragma once
#include <cstddef>

// ── A/V sync offset estimation (GCC-PHAT) ─────────────────────────────────────
// Estimate the time lag between two mono audio signals that recorded the same
// sound through different paths (e.g. the clean monitored mic vs the camera's
// scratch mic). Used to align a separately-captured clean audio track to the
// video before muxing, so the recorded take is lip-synced without a clapperboard.
//
// Method: generalized cross-correlation with phase transform (GCC-PHAT) — the
// cross-power spectrum is whitened by its magnitude so only timing survives,
// which keeps the correlation peak sharp under room reverb and level
// differences. Sub-sample accuracy via parabolic interpolation at the peak.
//
// Returns the lag in SECONDS: a positive value means signal `b` is delayed
// relative to `a` (b lags a) by that much — i.e. to align, b should be advanced
// (or a delayed). Searches within ±max_lag_s. Returns 0 on degenerate input.
float av_estimate_offset_seconds(const float* a, size_t na,
                                 const float* b, size_t nb,
                                 int sample_rate, float max_lag_s);
