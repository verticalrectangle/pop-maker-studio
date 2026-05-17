#pragma once
#include "app.h"
#include <vector>

// CTC forced alignment using wav2vec2-base-960h ONNX.
// Takes the 16 kHz mono PCM audio and the Whisper-derived word list,
// re-derives precise start/end times by running the audio through the
// acoustic model and Viterbi-decoding the known transcript against the
// per-frame character probabilities.
// Word timestamps are then snapped to MJPEG preview frame boundaries
// (round to nearest 1/proxy_fps second) when proxy_fps > 0.
// Returns an empty vector if the model file is missing or alignment fails;
// callers should keep the original Whisper timestamps in that case.
std::vector<WordEntry> forced_align(
    const std::vector<float>& audio16k,
    const std::vector<WordEntry>& whisper_words,
    double proxy_fps);
