#pragma once
// 4-stem source separation (htdemucs via ONNX Runtime; STFT/iSTFT and segment
// overlap-add in C++). Produces drums/bass/other/vocals WAVs for audio analysis v2.
#include <array>
#include <functional>
#include <string>

// True when the htdemucs model is installed in app_models_dir().
bool separate4_available();

// Separate audio_path into out_dir/{drums,bass,other,vocals}.wav (44.1 kHz stereo float).
// out_paths receives the four paths in that order. Returns false with err set when the
// model is missing or inference fails. Blocking; call from a worker thread.
bool separate_stems4(const std::string& audio_path, const std::string& out_dir,
                     std::array<std::string, 4>& out_paths,
                     const std::function<void(float)>& progress, std::string* err);

// Separate only [t0, t1) (source seconds) into out_dir/{drums,bass,other,vocals}.wav.
// Decodes the span (sample-accurate slice of the whole-file decode) and runs the
// same overlap-add; the wav lengths cover the span, with times relative to the
// SPAN START (callers add t0 back for source seconds). Blocking; worker thread.
bool separate_stems4_span(const std::string& audio_path, double t0, double t1,
                          const std::string& out_dir, std::array<std::string, 4>& out_paths,
                          const std::function<void(float)>& progress, std::string* err);
