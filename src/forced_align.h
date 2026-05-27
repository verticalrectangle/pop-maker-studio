#pragma once
#include "app.h"
#include <vector>
#include <utility>

// CTC forced alignment using wav2vec2-base-960h ONNX.
// Ports WhisperX's alignment algorithm to C++:
//   • stay/advance trellis (torchaudio forced-alignment tutorial)
//   • per-segment inference — audio window = exact whisper segment bounds
//   • NaN interpolation for characters not in the model vocabulary
//
// seg_bounds: (start_s, end_s) pairs for each whisper segment (0-based,
//   same time-base as whisper_words).  When empty, segments are reconstructed
//   from inter-word pauses.  Pass them for best accuracy.
//
// Returns an empty vector on failure; callers keep original Whisper timestamps.
std::vector<WordEntry> forced_align(
    const std::vector<float>&              audio16k,
    const std::vector<WordEntry>&          whisper_words,
    double                                 proxy_fps,
    const std::vector<std::pair<float,float>>& seg_bounds = {});
