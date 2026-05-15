#pragma once
// Convert a parsed RVC PyTorch checkpoint (PthModel) to an ONNX file that
// ONNX Runtime can load directly — no Python required.
//
// ONNX inputs:
//   phone  [1, T, phone_dim]  float32   — HuBERT phoneme features
//   f0     [1, T]             float32   — fundamental frequency in Hz
//   sid    [1]                int64     — speaker id
// ONNX output:
//   audio  [1, 1, M]          float32   — synthesised waveform samples
//
// A sidecar JSON is written alongside the .onnx file:
//   {"target_sr": <int>, "phone_dim": <int>}
#include "pth_reader.h"
#include <string>

// Convert PthModel to ONNX.
// Writes <out_path> and <out_path_without_ext>.json.
// Returns "" on success, error string on failure.
std::string pth_to_onnx(const PthModel& m, const std::string& out_path);
