#pragma once
// FaceTrack dump shared by the pms.face script binding and get_media_face IPC.
// Builds the SCRIPT_API.md §3.1 JSON from the face cache (face_cache.h):
// per-frame source times (real pts), landmarks normalised 0..1 in display
// orientation, per-frame eyeOpen, the unified blink signal (face_blink_of with
// the streaming open-eye baseline, exactly as get_face_blink), and the
// MediaPipe mesh connection sets (generated/face_mesh_edges.h).
#include <string>

struct FaceTrackDumpOptions {
    int max_frames = 4096;  // cap decoded frames (memory guard for long takes)
};

// face_track_dump_json(path, rot_q): full FaceTrack JSON object (no status
// wrapper). Returns false + err when the cache is missing/building or the
// models are unavailable. rot_q = quarter-turns CW the tracker rotated by.
bool face_track_dump_json(const std::string& path, int rot_q,
                          std::string& out_json, std::string* err,
                          const FaceTrackDumpOptions& opt = {});
