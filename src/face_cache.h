#pragma once
// Face landmark caches for recorded takes and any other video. A .face file in the
// media cache (cache_path(source, ".face")) holds per-frame landmarks, per-frame
// eye openness + blendshapes, and real source pts (built by face_track_build_cache
// on a background thread); playback, export and script clips read them by frame.
// Tiny data (~850 B/frame) — whole files load into an in-memory registry on first use.
#include "face_track.h"
#include <functional>
#include <string>

enum class FaceCacheStatus { None, Building, Ready, Failed };

// Kick a background build for this take if no usable cache exists. A cache
// built for a different rot_q (the user rotated the brick after recording)
// counts as unusable and is rebuilt. Safe to call every frame.
void face_cache_request(const std::string& take_path, int rot_q);

// Status + progress (0..1, valid while Building) for UI badges.
FaceCacheStatus face_cache_status(const std::string& take_path, float* progress);

// Landmarks for source time src_t (seconds into the take). Returns false
// while the cache is missing/building or when that frame has no face.
// Coords are RAW full-res take pixels (obs.w/h = take dimensions).
bool face_cache_obs(const std::string& take_path, int rot_q,
                    double src_t, FaceObs& out);

// Frame count for a ready cache (0 when missing/building).
int face_cache_frame_count(const std::string& take_path, int rot_q);
// Per-frame data: score (>0 = face), geometric eyeOpen, and the frame's
// source presentation time (seconds into the take; VFR sources report real
// times, not frame_index/fps). Returns false when unavailable.
bool face_cache_frame(const std::string& take_path, int rot_q, int fi,
                      FaceObs& out, double* src_time);

// Export prep: block until the cache is ready (building it if needed).
// Returns false on failure. progress is forwarded to the builder.
bool face_cache_ensure_sync(const std::string& take_path, int rot_q,
                            const std::function<void(float)>& progress);
