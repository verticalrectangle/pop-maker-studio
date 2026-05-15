#pragma once
// hubert_onnx.h — Convert a fairseq HuBERT base .pth checkpoint to ONNX.
#include "pth_reader.h"
#include <string>

// Convert a fairseq HuBERT base .pth checkpoint to ONNX.
// Returns "" on success, error string on failure.
std::string hubert_to_onnx(const PthModel& m, const std::string& out_path);
