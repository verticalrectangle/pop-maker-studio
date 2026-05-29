#pragma once
#include <string>

struct SceneResult {
    bool        ok = false;
    std::string description;
};

// Caption a single JPEG frame using the local Moondream2 ONNX model.
// Sessions are loaded lazily on first call.
// Returns SceneResult{ok=false} if models are missing or inference fails.
SceneResult caption_frame(const std::string& jpeg_path);
