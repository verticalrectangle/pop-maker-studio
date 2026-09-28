#pragma once
// Desktop face-skin segmentation: MediaPipe selfie_multiclass_256x256
// (Apache-2.0) converted to ONNX (see tools/convert_selfie_multiclass.py).
// Runs through ONNX Runtime on the CPU; no Python, no TFLite at runtime.
//
// Contract: models/selfie_multiclass_256x256.onnx with input "input_29"
// [1,256,256,3] RGB float32 in [0,1] and output "Identity" [1,256,256,6]
// per-class LOGITS in order background, hair, body-skin, face-skin,
// clothes, others (softmax over the last axis gives confidence maps).
//
// When the model file is missing every entry point below fails gracefully
// (false / empty), exactly like separate4_available() == false on th/demucs:
// skin-gated shaders fall back to their fixed YCbCr windows.
#include <array>
#include <functional>
#include <string>
#include <vector>

// True when the selfie_multiclass model is installed in app_models_dir().
bool skin_segment_available();

// Class order (matches the model's last-axis order).
static constexpr int SKIN_NCLASSES = 6;
static constexpr const char* SKIN_CLASS_NAMES[SKIN_NCLASSES] = {
    "background", "hair", "body_skin", "face_skin", "clothes", "others",
};

// Run the segmenter on RGB pixels (w*h*3, row-major, 0..255). On success
// fills `conf` with SKIN_NCLASSES per-pixel softmax confidences at the
// SOURCE resolution (the 256x256 output is bilinearly upsampled) and
// returns true. Blocking; call from a worker thread (a 256x256 ORT run is
// ~50-150 ms on CPU).
bool skin_segment_run(const uint8_t* rgb, int w, int h,
                      std::vector<float>& conf,  // out: w*h*SKIN_NCLASSES
                      std::string* err);

// Segment the image at `image_path` and write per-class 8-bit confidence
// PNGs (0..255 = softmax confidence of that class, NOT a hard argmax mask)
// at the source image's display resolution to
// out_dir/{background,hair,body_skin,face_skin,clothes,others}.png.
// Returns false with err set when the model is missing, the image cannot be
// decoded, or a mask cannot be written. Blocking; call from a worker thread.
bool skin_segment_image(const std::string& image_path, const std::string& out_dir,
                        std::array<std::string, SKIN_NCLASSES>& out_paths,
                        int& out_w, int& out_h,
                        const std::function<void(float)>& progress,
                        std::string* err);

// Face+body skin confidence (0..1) for one pixel, from a conf buffer as
// filled by skin_segment_run.
inline float skin_mask_conf(const float* conf_px) {
    return conf_px[2] + conf_px[3];
}
