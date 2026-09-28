#include "separate4.h"

#include "paths.h"

#include <filesystem>

namespace fs = std::filesystem;

static std::string model_path() { return (fs::path(app_models_dir()) / "htdemucs.onnx").string(); }

bool separate4_available() { return fs::exists(model_path()); }

bool separate_stems4(const std::string& audio_path, const std::string& out_dir,
                     std::array<std::string, 4>& out_paths,
                     const std::function<void(float)>& progress, std::string* err) {
    (void)audio_path;
    (void)out_dir;
    (void)out_paths;
    (void)progress;
    if (err) *err = "4-stem separation is not built into this binary yet (htdemucs runtime lands with th/demucs)";
    return false;
}
