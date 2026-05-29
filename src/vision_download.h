#pragma once
#include <string>

// On-demand download of Moondream2 q4 ONNX models (~1.1 GB total).
// Checks the local HuggingFace cache first; falls back to a direct download.
// All functions are thread-safe.

bool        vision_models_ready();       // all 3 model files present in models dir
bool        vision_download_start();     // start download; returns false if already running
bool        vision_download_running();
bool        vision_download_done();
float       vision_download_progress();  // 0.0–1.0 weighted by file size
std::string vision_download_message();  // human-readable current step
std::string vision_download_error();    // non-empty on failure
void        vision_download_cancel();
