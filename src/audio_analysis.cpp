#include "audio_analysis.h"

#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

using json = nlohmann::json;

static const char* const k_hit_names[(int)HitKind::Count] = {"kick", "snare", "hat", "bass", "other", "vocal"};
static const char* const k_env_names[(int)EnvKind::Count] = {"mix", "drums", "bass", "other", "vocals"};
static const char* const k_stem_names[4] = {"drums", "bass", "other", "vocals"};

const char* hit_kind_name(HitKind k) { return k_hit_names[(int)k]; }
const char* env_kind_name(EnvKind k) { return k_env_names[(int)k]; }

static json to_json_obj(const AudioAnalysis& a) {
    json j;
    j["version"] = a.version;
    j["source"] = a.source;
    j["duration"] = a.duration;
    j["bpm"] = a.bpm;
    j["beats"] = a.beats;
    j["downbeats"] = a.downbeats;
    json hits = json::object();
    for (int k = 0; k < (int)HitKind::Count; k++) {
        json arr = json::array();
        for (const AudioHit& h : a.hits[k]) arr.push_back({{"t", h.t}, {"s", h.s}});
        hits[k_hit_names[k]] = std::move(arr);
    }
    j["hits"] = std::move(hits);
    j["fps"] = a.fps;
    j["env_start"] = a.env_start;
    json env = json::object();
    for (int k = 0; k < (int)EnvKind::Count; k++) env[k_env_names[k]] = a.env[k];
    j["env"] = std::move(env);
    j["spectrum_bands"] = a.spectrum_bands;
    json spec = json::array();
    const int bands = std::max(1, a.spectrum_bands);
    for (size_t f = 0; f + bands <= a.spectrum.size(); f += bands) {
        spec.push_back(std::vector<int>(a.spectrum.begin() + f, a.spectrum.begin() + f + bands));
    }
    j["spectrum"] = std::move(spec);
    j["lines"] = a.lines;
    json words = json::array();
    for (const AnalysisWord& w : a.words) {
        words.push_back({{"w", w.w}, {"line", w.line}, {"i", w.i}, {"t0", w.t0}, {"t1", w.t1}, {"conf", w.conf}});
    }
    j["words"] = std::move(words);
    json stems = json::object();
    for (int k = 0; k < 4; k++) {
        if (!a.stems[k].empty()) stems[k_stem_names[k]] = a.stems[k];
    }
    j["stems"] = std::move(stems);
    return j;
}

std::string audio_analysis_to_json(const AudioAnalysis& a) { return to_json_obj(a).dump(); }

bool audio_analysis_save_json(const AudioAnalysis& a, const std::string& path, std::string* err) {
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        if (err) *err = "cannot write " + path;
        return false;
    }
    f << to_json_obj(a).dump();
    return (bool)f;
}

bool audio_analysis_load_json(const std::string& path, AudioAnalysis& out, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (err) *err = "cannot read " + path;
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    json j = json::parse(ss.str(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        if (err) *err = "invalid JSON in " + path;
        return false;
    }
    try {
        AudioAnalysis a;
        a.version = j.value("version", 2);
        a.source = j.value("source", std::string());
        a.duration = j.value("duration", 0.0);
        a.bpm = j.value("bpm", 0.f);
        a.beats = j.value("beats", std::vector<double>());
        a.downbeats = j.value("downbeats", std::vector<double>());
        if (j.contains("hits")) {
            for (int k = 0; k < (int)HitKind::Count; k++) {
                if (!j["hits"].contains(k_hit_names[k])) continue;
                for (const json& h : j["hits"][k_hit_names[k]]) a.hits[k].push_back({h.at("t").get<double>(), h.at("s").get<float>()});
            }
        }
        a.fps = j.value("fps", 60);
        a.env_start = j.value("env_start", 0.0);
        if (j.contains("env")) {
            for (int k = 0; k < (int)EnvKind::Count; k++) {
                if (j["env"].contains(k_env_names[k])) a.env[k] = j["env"][k_env_names[k]].get<std::vector<float>>();
            }
        }
        if (j.contains("spectrum") && j["spectrum"].is_array() && !j["spectrum"].empty()) {
            a.spectrum_bands = j.value("spectrum_bands", (int)j["spectrum"][0].size());
            a.spectrum.reserve(j["spectrum"].size() * a.spectrum_bands);
            for (const json& row : j["spectrum"]) {
                for (const json& v : row) a.spectrum.push_back((uint8_t)std::clamp(v.get<int>(), 0, 99));
            }
        }
        a.lines = j.value("lines", std::vector<std::string>());
        if (j.contains("words")) {
            for (const json& w : j["words"]) {
                a.words.push_back({w.at("w").get<std::string>(), w.value("line", 0), w.value("i", 0),
                                   w.at("t0").get<double>(), w.at("t1").get<double>(), w.value("conf", 1.f)});
            }
        }
        if (j.contains("stems")) {
            for (int k = 0; k < 4; k++) a.stems[k] = j["stems"].value(k_stem_names[k], std::string());
        }
        out = std::move(a);
        return true;
    } catch (const std::exception& e) {
        if (err) *err = std::string("bad analysis schema: ") + e.what();
        return false;
    }
}
