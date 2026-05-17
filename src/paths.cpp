#include "paths.h"
#include <unistd.h>
#include <filesystem>
namespace fs = std::filesystem;
std::string app_models_dir() {
    char buf[4096] = {};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf)-1);
    if (n <= 0) return "models";
    return (fs::path(buf).parent_path() / "models").string();
}

std::string wav2vec2_ctc_path() {
    return (fs::path(app_models_dir()) / "wav2vec2_ctc.onnx").string();
}
