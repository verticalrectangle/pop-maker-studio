#pragma once
#include <string>
#include <functional>

std::string separate_model_path();
bool        separate_model_exists();

// Separate input_path into vocals_out + instrumental_out WAV files.
// Model is auto-downloaded if absent.
// clip_in / clip_dur (seconds): when > 0, only the source region [clip_in, clip_in+clip_dur]
// is decoded and separated — avoids processing unused parts of a long source.
// Returns empty string on success, error message on failure.
std::string separate_run(
    const std::string& input_path,
    const std::string& vocals_out,
    const std::string& instrumental_out,
    std::function<void(float, const std::string&)> on_progress = nullptr,
    float clip_in  = 0.f,
    float clip_dur = 0.f);
