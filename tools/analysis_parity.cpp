// analysis-parity: offline driver for audio_analysis_run (acceptance only).
// Usage: analysis-parity <clip.wav> <out.json> [--stems-dir DIR]
// Lyrics come from <out>.lyrics.json (a JSON list of strings; written by
// tools/analysis_parity.py). Prints progress + summary to stdout.
#include "audio_analysis.h"
#include "json.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using json = nlohmann::json;

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <clip.wav> <out.json> [--stems-dir DIR]\n", argv[0]);
        return 2;
    }
    std::string clip = argv[1], outp = argv[2], stems;
    for (int i = 3; i + 1 < argc; i++)
        if (std::string(argv[i]) == "--stems-dir") stems = argv[++i];

    AudioAnalysisOptions opt;
    opt.separate_stems = stems.empty();  // injected stems skip separation entirely
    opt.stems_dir = stems;
    std::string lpath = outp + ".lyrics.json";
    {
        std::ifstream f(lpath);
        if (f) {
            try {
                json lj = json::parse(f);
                for (auto& l : lj) opt.lyrics.push_back(l.get<std::string>());
            } catch (...) {}
        }
    }
    if (opt.lyrics.empty()) {
        // Fall back to lines.json next to the output dir (parity script layout).
        std::string alt = std::string(outp.begin(), outp.begin() + (outp.rfind('/') + 1)) +
                          "lines.json";
        std::ifstream f(alt);
        if (f) {
            try {
                json lj = json::parse(f);
                for (auto& l : lj) opt.lyrics.push_back(l.get<std::string>());
            } catch (...) {}
        }
    }

    AudioAnalysis a;
    std::string err;
    bool ok = audio_analysis_run(
        clip, opt, a,
        [](float p, const char* s) {
            printf("[%5.1f%%] %s\n", p * 100, s);
            fflush(stdout);
        },
        &err);
    if (!ok) {
        fprintf(stderr, "analysis failed: %s\n", err.c_str());
        return 1;
    }
    if (!audio_analysis_save_json(a, outp, &err)) {
        fprintf(stderr, "save failed: %s\n", err.c_str());
        return 1;
    }
    printf("bpm=%.2f beats=%d downbeats=%d words=%d duration=%.2f\n", a.bpm,
           (int)a.beats.size(), (int)a.downbeats.size(), (int)a.words.size(), a.duration);
    const char* kn[] = {"kick", "snare", "hat", "bass", "other", "vocal"};
    for (int k = 0; k < 6; k++) printf("  %s: %d hits\n", kn[k], (int)a.hits[k].size());
    return 0;
}
