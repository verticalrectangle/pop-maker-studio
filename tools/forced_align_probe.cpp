// Standalone harness for forced_align(), per the project's forced-align
// discipline: verify alignment changes against the REAL wav2vec2 model on a real
// stem, without re-running the whole transcription pipeline.
//
// It decodes a vocal stem to 16 kHz mono (via ffmpeg), loads whisper words and
// segment bounds from the pipeline's cached JSON, runs forced_align, and prints
// the first words. To isolate a change, run it once before and once after the
// edit on the SAME inputs and diff: the only words that should move are the ones
// the change targets.
//
// Usage: forced_align_probe <stem.wav> <words.json> <segments.json> [N]
#include "forced_align.h"
#include "json.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using json = nlohmann::json;

// Shell out to ffmpeg → raw f32le 16 kHz mono on stdout (no avcodec linking).
static std::vector<float> decode_16k(const std::string& path) {
    std::string cmd = "ffmpeg -v error -i \"" + path +
                      "\" -ar 16000 -ac 1 -f f32le pipe:1";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return {};
    std::vector<float> pcm;
    float buf[4096];
    size_t n;
    while ((n = fread(buf, sizeof(float), 4096, p)) > 0)
        pcm.insert(pcm.end(), buf, buf + n);
    pclose(p);
    return pcm;
}

static json load_json(const std::string& path) {
    std::ifstream f(path);
    if (!f) { fprintf(stderr, "cannot open %s\n", path.c_str()); exit(1); }
    return json::parse(f);
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr,
                "usage: %s <stem.wav> <words.json> <segments.json> [N]\n",
                argv[0]);
        return 2;
    }
    int N = (argc >= 5) ? atoi(argv[4]) : 12;

    std::vector<float> audio = decode_16k(argv[1]);
    fprintf(stderr, "decoded %zu samples (%.1fs)\n",
            audio.size(), audio.size() / 16000.0);
    if (audio.empty()) { fprintf(stderr, "decode failed\n"); return 1; }

    json wj = load_json(argv[2]);
    std::vector<WordEntry> words;
    for (auto& w : wj) {
        WordEntry we;
        we.text  = w.contains("word") ? w["word"].get<std::string>()
                                       : w.value("text", std::string());
        we.start = w.value("start", 0.f);
        we.end   = w.value("end",   0.f);
        words.push_back(we);
    }

    json sj = load_json(argv[3]);
    std::vector<std::pair<float, float>> seg_bounds;
    for (auto& s : sj)
        seg_bounds.push_back({s.value("start", 0.f), s.value("end", 0.f)});

    fprintf(stderr, "words=%zu segs=%zu\n", words.size(), seg_bounds.size());

    std::vector<WordEntry> out = forced_align(audio, words, 0.0, seg_bounds);
    if (out.empty()) { fprintf(stderr, "forced_align returned empty\n"); return 1; }

    printf("idx     start      end   word\n");
    for (int i = 0; i < (int)out.size() && i < N; ++i)
        printf("%3d  %8.3f %8.3f   %s\n",
               i, out[i].start, out[i].end, out[i].text.c_str());
    return 0;
}
