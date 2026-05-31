#pragma once
#include <atomic>
#include <string>
std::string app_models_dir();    // <binary_dir>/models
std::string wav2vec2_ctc_path(); // <binary_dir>/models/wav2vec2_ctc.onnx

// Set true by app_shutdown; background threads check this to exit early.
extern std::atomic<bool> g_shutdown;
