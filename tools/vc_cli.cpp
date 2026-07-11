// vc_cli.cpp — tiny CLI for pop-maker-studio voice conversion (uses HuBERT)
// Build: cd pop-maker-studio/build && ninja vc_cli
#include "vc_onnx.h"
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <input.wav> <voice.onnx> [output.wav] [f0_semitones]\n", argv[0]);
        return 2;
    }
    std::string in    = argv[1];
    std::string voice = argv[2];
    std::string out   = argc > 3 ? argv[3] : (in.substr(0, in.rfind('.')) + "_converted.wav");
    int f0            = argc > 4 ? atoi(argv[4]) : 0;

    fprintf(stderr, "Converting: %s -> %s (HuBERT pipeline)\n", in.c_str(), out.c_str());
    std::string err = vc_onnx_convert(in, voice, out, f0, false,
        [](float p, const std::string& msg) {
            fprintf(stderr, "  %.0f%% %s\n", p * 100.f, msg.c_str());
        });
    if (!err.empty()) {
        fprintf(stderr, "ERROR: %s\n", err.c_str());
        return 1;
    }
    fprintf(stderr, "Done: %s\n", out.c_str());
    return 0;
}
