#pragma once
// Read a PyTorch .pth checkpoint (zip + pickle format) without Python or libtorch.
// Extracts the weight dict and config list needed to build a VITS/RVC ONNX model.
#include <string>
#include <vector>
#include <unordered_map>

// ── Tensor metadata ───────────────────────────────────────────────────────────

struct TensorMeta {
    std::string dtype;           // "f16", "f32", "i64", "i32"
    int64_t     storage_offset;  // in elements
    std::string storage_file;    // "0", "1", "12" → data/{n} in zip
    int64_t     num_elements;    // total elements in storage
    std::vector<int64_t> shape;
    std::vector<int64_t> stride;
};

// ── RVC model config (extracted from cpt['config']) ──────────────────────────

struct RvcConfig {
    int spec_channels        = 1025;
    int segment_size         = 32;
    int inter_channels       = 192;
    int hidden_channels      = 192;
    int filter_channels      = 768;
    int n_heads              = 2;
    int n_layers             = 6;
    int kernel_size          = 3;
    float p_dropout          = 0.f;
    std::string resblock     = "1";
    std::vector<int> resblock_kernel_sizes  = {3, 7, 11};
    std::vector<std::vector<int>> resblock_dilation_sizes = {{1,3,5},{1,3,5},{1,3,5}};
    std::vector<int> upsample_rates         = {10, 10, 2, 2};
    int upsample_initial_channel = 512;
    std::vector<int> upsample_kernel_sizes  = {16, 16, 4, 4};
    int n_speakers           = 1;
    int gin_channels         = 256;
    int sr                   = 40000;
    int phone_dim            = 768;
};

// ── PthModel ─────────────────────────────────────────────────────────────────

struct PthModel {
    RvcConfig   config;
    // Metadata for each weight tensor.  Actual float data is NOT stored here;
    // use pth_load_tensor() to read it on demand.
    std::unordered_map<std::string, TensorMeta> tensors;
    // Temp directory created by pth_open() — caller must pth_close() when done.
    std::string tmpdir;
    std::string err;
};

// Open a .pth file: extract zip to a temp dir, parse pickle, return metadata.
// Call pth_close() when done to remove the temp dir.
PthModel pth_open(const std::string& path);
void     pth_close(PthModel& m);

// Read one tensor's data from the already-extracted tmpdir, converting to float32.
// Returns empty on error.
std::vector<float> pth_load_tensor(const PthModel& m, const std::string& name);

// Convenience: load all tensors into a flat map {name → float32 data}.
// Only practical for models that fit in RAM (most RVC models are ~200 MB as f16).
std::unordered_map<std::string, std::vector<float>>
pth_load_all(PthModel& m);
