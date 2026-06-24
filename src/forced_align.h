#pragma once
#include "app.h"
#include <vector>
#include <utility>

// CTC forced alignment using wav2vec2-base-960h ONNX.
// stay/advance trellis algorithm (torchaudio forced-alignment tutorial):
//   • per-segment inference — audio window = whisper segment bounds, but the
//     window START is clamped to the first word's DTW onset (a loose segment
//     boundary would let the CTC drag the first word back into pre-onset audio)
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
