// separate4_cli.cpp — tiny CLI for 4-stem htdemucs separation.
// Build: cmake --build build --parallel 3 --target separate4_cli
// Usage: separate4_cli <input.wav> <out_dir>
#include "separate4.h"
#include <array>
#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <input audio> <out_dir>\n", argv[0]);
        return 2;
    }
    std::array<std::string, 4> outs;
    std::string err;
    bool ok = separate_stems4(argv[1], argv[2], outs,
        [](float p) { fprintf(stderr, "  %.0f%%\n", p * 100.f); }, &err);
    if (!ok) { fprintf(stderr, "ERROR: %s\n", err.c_str()); return 1; }
    for (auto& o : outs) printf("%s\n", o.c_str());
    return 0;
}
