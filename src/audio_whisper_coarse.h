#pragma once
// Whisper coarse pass for audio analysis v2 word alignment.
//
// The greedy-CTC coarse windows drifted whole lines by seconds (line-5 window
// landed ~5.8 s late: the quantized wav2vec2 model hallucinates long runs of
// '|'/vowel tokens in sung regions, so fuzzy token matching anchors lines to
// the wrong span). Whisper (large-v3-turbo, already a models/ dependency for
// transcription) transcribes sung vocals reliably — on clip.wav it recovers
// all 7 lyric lines with correct segment boundaries, including the 24.3–30 s
// instrumental gap the greedy decode filled with garbage.
//
// whisper_coarse_windows() runs whisper.cpp in-process on the 16 kHz vocal
// audio, then maps each lyric line (in order) onto the decoded word stream
// with an order-constrained fuzzy match: each line's window is derived from
// the matched word span (start of first matched word − cushion, end of last
// matched word + cushion), never before the previous line's end. Lines that
// match nothing fall back to even splits. Returns false with err set when
// whisper fails or the model is missing (caller falls back to even splits).
#include <functional>
#include <string>
#include <utility>
#include <vector>

bool whisper_coarse_windows(const std::vector<float>& audio16k,
                            const std::vector<std::string>& lyrics,
                            std::vector<std::pair<float, float>>& wins_out,
                            const std::function<void(float, const char*)>& progress,
                            std::string* err);
