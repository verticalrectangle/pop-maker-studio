#pragma once
// FaceTrack dump shared by the pms.face script binding and get_media_face IPC.
// Builds the SCRIPT_API.md §3.1 JSON from the face cache (face_cache.h):
// per-frame landmarks (normalised 0..1 in display orientation), eyeOpen via
// face_metrics.h, blink = max(blendshape blink, 1 - eyeOpen/baseline).
#include <string>

struct FaceTrackDumpOptions {
    int max_frames = 4096;  // cap decoded frames (memory guard for long takes)
};

// face_track_dump_json(path, rot_q): full FaceTrack JSON object (no status
// wrapper). Returns false + err when the cache is missing/building, the
// models are unavailable, or decode fails. rot_q = quarter-turns CW.
bool face_track_dump_json(const std::string& path, int rot_q,
                          std::string& out_json, std::string* err,
                          const FaceTrackDumpOptions& opt = {});
