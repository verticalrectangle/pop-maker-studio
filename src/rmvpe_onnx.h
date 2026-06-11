#pragma once
// RMVPE neural pitch tracker via ONNX Runtime.
// Far more robust than YIN: no octave jumps, clean voiced/unvoiced decisions.
// Model: rmvpe.onnx (input log-mel [1,128,T], output salience [1,T,360]).
#include <string>
#include <vector>

std::string rmvpe_onnx_path();
bool        rmvpe_onnx_exists();

// Estimate F0 from 16 kHz mono audio. Returns one value per 10 ms frame
// (0.0 = unvoiced). Empty on error.
std::vector<float> rmvpe_f0(const std::vector<float>& wav16k);
