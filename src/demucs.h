#pragma once
#include <string>
#include <functional>

std::string demucs_model_path();
bool        demucs_model_exists();

// Download the HTDemucs ONNX model (~289 MB). Synchronous; call from a thread.
// Returns empty string on success, error message on failure.
std::string demucs_download(
    std::function<void(float, const std::string&)> on_progress = nullptr);

// Separate input_path into vocals_out + instrumental_out WAV files.
// Model is auto-downloaded if absent.
// Returns empty string on success, error message on failure.
std::string demucs_separate(
    const std::string& input_path,
    const std::string& vocals_out,
    const std::string& instrumental_out,
    std::function<void(float, const std::string&)> on_progress = nullptr);
