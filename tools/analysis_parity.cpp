// analysis-parity: offline driver for audio_analysis_run (acceptance only).
// Usage: analysis-parity <clip.wav> <out.json> [--stems-dir DIR] [--separate]
//        [--range t0,t1]
// Lyrics come from <out>.lyrics.json: a JSON list whose items are either
// plain strings or {"text": ..., "t0": ..., "t1": ...} objects with a coarse
// window in source seconds (written by tools/analysis_parity.py). --range
// restricts decode/separate/analyse/normalise to [t0,t1) source seconds
// (times stay absolute). Prints progress + summary to stdout.
#include "audio_analysis.h"
#include "json.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using json = nlohmann::json;

static void push_lyric(AudioAnalysisOptions& opt, const json& l) {
    LyricLine ll;
    if (l.is_string()) {
        ll.text = l.get<std::string>();
    } else if (l.is_object()) {
        ll.text = l.value("text", "");
        if (l.contains("t0") && l.contains("t1")) {
            double t0 = l.value("t0", 0.0), t1 = l.value("t1", 0.0);
            if (t1 > t0) {
                ll.has_window = true;
                ll.w0 = t0;
                ll.w1 = t1;
            }
        }
    }
    if (!ll.text.empty()) opt.lyrics.push_back(std::move(ll));
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <clip.wav> <out.json> [--stems-dir DIR] [--separate] [--range t0,t1]\n", argv[0]);
        return 2;
    }
    std::string clip = argv[1], outp = argv[2], stems;
    bool separate = false;
    bool has_range = false;
    double range_t0 = 0.0, range_t1 = 0.0;
    for (int i = 3; i < argc; i++) {
        if (std::string(argv[i]) == "--stems-dir" && i + 1 < argc) stems = argv[++i];
        else if (std::string(argv[i]) == "--separate") separate = true;
        else if (std::string(argv[i]) == "--range" && i + 1 < argc) {
            std::string r = argv[++i];
            size_t c = r.find(',');
            if (c != std::string::npos) {
                range_t0 = std::stod(r.substr(0, c));
                range_t1 = std::stod(r.substr(c + 1));
                has_range = range_t1 > range_t0 && range_t0 >= 0.0;
            }
        }
    }

    AudioAnalysisOptions opt;
    // Injected stems skip separation entirely (a reuse feature); --separate
    // forces the real C++ htdemucs path even when a stems dir exists.
    opt.separate_stems = separate || stems.empty();
    opt.stems_dir = separate ? std::string() : stems;
    opt.has_range = has_range;
    opt.range_t0 = range_t0;
    opt.range_t1 = range_t1;
    std::string lpath = outp + ".lyrics.json";
    {
        std::ifstream f(lpath);
        if (f) {
            try {
                json lj = json::parse(f);
                for (auto& l : lj) push_lyric(opt, l);
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
                for (auto& l : lj) push_lyric(opt, l);
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
