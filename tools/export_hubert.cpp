// export_hubert.cpp — CLI tool: convert hubert_base.pt → hubert.onnx
//
// Usage:
//   ./export-hubert hubert_base.pt hubert.onnx
//
// Exits 0 on success, 1 on error.
#include "pth_reader.h"
#include "hubert_onnx.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>

int main(int argc, char* argv[])
{
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <hubert_base.pt> <output.onnx>\n", argv[0]);
        return 1;
    }

    const std::string in_path  = argv[1];
    const std::string out_path = argv[2];

    fprintf(stdout, "Opening %s ...\n", in_path.c_str());
    fflush(stdout);

    PthModel m = pth_open(in_path);
    if (!m.err.empty()) {
        fprintf(stderr, "Error opening .pth: %s\n", m.err.c_str());
        return 1;
    }

    fprintf(stdout, "Loaded %zu tensors.\n", m.tensors.size());

    // Print all tensor names to aid debugging if something is missing.
    if (m.tensors.empty()) {
        fprintf(stderr, "ERROR: no tensors found in checkpoint.\n");
        pth_close(m);
        return 1;
    }

    // Show a brief summary of available keys (first 20, then count).
    {
        std::vector<std::string> names;
        names.reserve(m.tensors.size());
        for (auto& kv : m.tensors) names.push_back(kv.first);
        std::sort(names.begin(), names.end());

        size_t show = names.size() < 20 ? names.size() : 20;
        fprintf(stdout, "First %zu tensor names:\n", show);
        for (size_t i = 0; i < show; i++)
            fprintf(stdout, "  %s\n", names[i].c_str());
        if (names.size() > show)
            fprintf(stdout, "  ... (%zu more)\n", names.size() - show);
        fflush(stdout);
    }

    fprintf(stdout, "Exporting ONNX to %s ...\n", out_path.c_str());
    fflush(stdout);

    std::string err = hubert_to_onnx(m, out_path);
    pth_close(m);

    if (!err.empty()) {
        fprintf(stderr, "Export failed: %s\n", err.c_str());
        // If the error is about a missing tensor, print all available tensor names
        // to help diagnose key-name mismatches.
        if (err.find("missing tensor") != std::string::npos ||
            err.find("no tensor")     != std::string::npos)
        {
            // Re-open to dump names (m was already closed, so we do a fresh open).
            PthModel m2 = pth_open(in_path);
            if (m2.err.empty()) {
                fprintf(stderr, "\nAll available tensor names:\n");
                std::vector<std::string> names;
                for (auto& kv : m2.tensors) names.push_back(kv.first);
                std::sort(names.begin(), names.end());
                for (auto& n : names) fprintf(stderr, "  %s\n", n.c_str());
                pth_close(m2);
            }
        }
        return 1;
    }

    fprintf(stdout, "Success: wrote %s\n", out_path.c_str());
    return 0;
}
