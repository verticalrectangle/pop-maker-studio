// Script runtime: QuickJS-ng host, ES module loader, `pms` bindings
// (docs/SCRIPT_API.md §2–§3). Drawing is script_context2d.* (Skia).
//
// Loading model (mirrors qjs.c eval_buf): entry + every import compiled
// with JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY, linked with
// JS_ResolveModule, executed once with JS_EvalFunction. setup() runs at
// build; render(f) runs per frame. The entry namespace
// (JS_GetModuleNamespace) provides setup/render.
//
// Purity: Math.random / Date.now + timer stubs (requestAnimationFrame,
// setTimeout/setInterval/clearTimeout/clearInterval) throw; console.log
// routes to the script log.
//
// Identity stability: pms.audio / pms.words / pms.lines / pms.json(path)
// return the SAME frozen JSValue until the underlying data changes (audio
// epoch bump / file mtime change). pms.json entries memoise {mtime, value};
// face/image objects memoise per path.
#include "script_runtime.h"
#include "script_context2d.h"
#include "app.h"
#include "audio_analysis.h"
#include "face_cache.h"
#include "script_face.h"

#include "quickjs.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unordered_map>
#include <unistd.h>

namespace fs = std::filesystem;

// ── Paths ────────────────────────────────────────────────────────────────

static std::string asset_root() {
    if (const char* e = std::getenv("PMS_ASSET_ROOT")) {
        if (fs::exists(e)) return e;
    }
    char buf[4096] = {};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    std::string dir = n > 0 ? fs::path(std::string(buf, (size_t)n)).parent_path().string() : ".";
    const char* cands[] = {".", "..", "../..", nullptr};
    for (int i = 0; cands[i]; ++i) {
        std::string p = (fs::path(dir) / cands[i] / "assets").string();
        std::error_code ec;
        if (fs::exists(p, ec)) return fs::canonical(p, ec).string();
    }
    return dir;
}

// Resolve `pms:fonts/<rel>` to assets/fonts/<rel>: exact path first, then a
// case-insensitive basename match under assets/fonts/display/ (preset font ids
// are sanitized lowercase basenames, e.g. "anton" for Anton.ttf).
static std::string script_resolve_font(const std::string& rel) {
    std::string root = asset_root();
    {
        std::string exact = root + "/fonts/" + rel;
        std::error_code ec;
        if (fs::exists(exact, ec)) return fs::canonical(exact, ec).string();
    }
    std::string base = fs::path(rel).filename().string();
    std::string low;
    for (char c : base) low += (char)tolower((unsigned char)c);
    std::string dir = root + "/fonts/display";
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::string fn = it->path().filename().string();
        std::string flow;
        for (char c : fn) flow += (char)tolower((unsigned char)c);
        if (flow == low) return it->path().string();
    }
    return "";
}

std::string script_resolve_spec(const std::string& spec, const std::string& referrer,
                                const std::string& entry_dir) {
    if (spec.rfind("pms:", 0) == 0) {
        std::string tail = spec.substr(4);
        std::string base = asset_root() + "/scripts/";
        if (tail.rfind("typography/", 0) == 0) return base + tail + ".js";
        if (tail == "rhythm" || tail == "text") return base + "std/" + tail + ".js";
        if (tail.rfind("fonts/", 0) == 0) return script_resolve_font(tail.substr(6));
        return "";
    }
    fs::path ref(referrer.empty() ? entry_dir : fs::path(referrer).parent_path().string());
    fs::path p = ref / spec;
    if (p.extension().empty()) p += ".js";
    std::error_code ec;
    auto c = fs::canonical(p, ec);
    if (ec) return "";
    return c.string();
}

static uint64_t file_mtime_ns(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return 0;
    return (uint64_t)st.st_mtim.tv_sec * 1000000000ull + (uint64_t)st.st_mtim.tv_nsec;
}

static bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

uint64_t script_audio_epoch(const AppState& state) {
    return (uint64_t)state.audio_analysis.get();
}

// ── Impl ─────────────────────────────────────────────────────────────────

struct ScriptRuntime::Impl {
    JSRuntime* rt = nullptr;
    JSContext* ctx = nullptr;
    JSValue entry_ns = JS_UNDEFINED;
    JSValue pms_obj = JS_UNDEFINED;
    JSValue canvas_obj = JS_UNDEFINED;  // the Context2D (script_context2d.h)
    std::string entry_path;
    std::string entry_dir;
    const AppState* state = nullptr;
    std::unordered_map<std::string, uint64_t> watched;
    // audio memo
    uint64_t memo_audio_epoch = 0;
    std::string memo_audio_key;
    JSValue memo_audio = JS_UNDEFINED;
    JSValue memo_words = JS_UNDEFINED;
    JSValue memo_lines = JS_UNDEFINED;
    double last_audio_off = 0.0;
    // Transcript fallback behind pms.words when pms.audio has no words:
    // project transcript (AppState::words_cache, source seconds of
    // state.audio_path) with lyrics_edits applied, mapped to timeline seconds
    // through the timeline clip that plays that source (same mapping as
    // pms.audio: offset = in_point - start of the first Audio/Video clip
    // whose text/source_id equals the source). Memoised; rebuilt only when
    // the words, edits or mapping change.
    uint64_t memo_words_n = 0;
    double memo_words_first = 0.0, memo_words_last = 0.0;
    size_t memo_edits_n = 0;
    std::string memo_words_src;
    std::string memo_trans_key;
    double memo_trans_off = 0.0;
    bool memo_valid = false;
    struct JsonEntry { uint64_t mtime = 0; JSValue value = JS_UNDEFINED; };
    std::unordered_map<std::string, JsonEntry> json_cache;
    std::unordered_map<std::string, JSValue> face_cache;
    std::unordered_map<std::string, JSValue> image_cache;
    std::vector<std::string> log_tail;
    std::vector<std::string> face_requests;
    std::string post_frag;
    ScriptUniforms post_uniforms;
    JSValue setup_fn = JS_UNDEFINED;
    JSValue render_fn = JS_UNDEFINED;
};

static ScriptRuntime::Impl* self_of(JSContext* ctx) {
    return (ScriptRuntime::Impl*)JS_GetContextOpaque(ctx);
}

static std::string js_exc_text(JSContext* ctx) {
    // Only call when JS_HasException is true: JS_GetException with no pending
    // exception returns undefined, and JS_GetPropertyStr on that segfaults
    // inside QuickJS (find_own_property). Callers must gate on HasException;
    // this double-checks for defence in depth.
    if (!JS_HasException(ctx)) return "unknown error";
    JSValue e = JS_GetException(ctx);
    if (JS_IsUndefined(e) || JS_IsNull(e) || JS_IsException(e)) {
        if (!JS_IsException(e)) JS_FreeValue(ctx, e);
        return "unknown error";
    }
    // Non-object throws (throw "x", throw 42) have no properties: stringify
    // only, never touch .stack.
    if (!JS_IsObject(e)) {
        const char* s = JS_ToCString(ctx, e);
        std::string out = s ? s : "unknown error";
        if (s) JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, e);
        return out;
    }
    const char* s = JS_ToCString(ctx, e);
    std::string out = s ? s : "unknown error";
    if (s) JS_FreeCString(ctx, s);
    JSValue stack = JS_GetPropertyStr(ctx, e, "stack");
    if (JS_IsObject(stack) || JS_IsString(stack)) {
        const char* ss = JS_ToCString(ctx, stack);
        if (ss) { out += "\n"; out += ss; JS_FreeCString(ctx, ss); }
    }
    JS_FreeValue(ctx, stack);
    JS_FreeValue(ctx, e);
    return out;
}

ScriptRuntime::ScriptRuntime() : impl_(new Impl) {}
ScriptRuntime::~ScriptRuntime() { teardown(); }


// ── Module loader hooks ──────────────────────────────────────────────────

static char* script_normalize(JSContext* ctx, const char* base_name,
                              const char* name, void* opaque) {
    ScriptRuntime::Impl* self = (ScriptRuntime::Impl*)opaque;
    (void)ctx;
    std::string r = script_resolve_spec(name ? name : "",
                                        base_name ? base_name : "",
                                        self->entry_dir);
    if (r.empty()) return nullptr;
    return strdup(r.c_str());
}

static JSModuleDef* script_loader(JSContext* ctx, const char* module_name,
                                  void* opaque) {
    ScriptRuntime::Impl* self = (ScriptRuntime::Impl*)opaque;
    std::string src;
    if (!read_file(module_name, src)) {
        JS_ThrowReferenceError(ctx, "could not load module '%s'", module_name);
        return nullptr;
    }
    self->watched[module_name] = file_mtime_ns(module_name);
    JSValue func = JS_Eval(ctx, src.c_str(), src.size(), module_name,
                           JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(func)) return nullptr;
    JSModuleDef* m = (JSModuleDef*)JS_VALUE_GET_PTR(func);
    JS_FreeValue(ctx, func);
    return m;
}

// ── pms.audio mapping ────────────────────────────────────────────────────
// Event times shift to timeline seconds (kept even outside the clip range);
// env/spectrum re-index so index 0 = timeline 0 at fps (drop leading frames,
// zero-pad when the clip starts before source 0); offset = source seconds at
// timeline 0; duration stays the source duration.

static double audio_offset_for(const AppState& state, const AudioAnalysis& a,
                               bool& found) {
    found = false;
    if (a.source.empty()) return 0.0;
    for (auto& tr : state.tracks) {
        for (auto& cl : tr.clips) {
            if (cl.clip_type != ClipType::Audio && cl.clip_type != ClipType::Video)
                continue;
            if (cl.text == a.source || cl.source_id == a.source) {
                found = true;
                return (double)cl.in_point - (double)cl.start;
            }
        }
    }
    return 0.0;
}

static JSValue new_float_array(JSContext* ctx, const std::vector<float>& v,
                               double shift) {
    JSValue arr = JS_NewArray(ctx);
    for (uint32_t i = 0; i < v.size(); ++i)
        JS_DefinePropertyValueUint32(ctx, arr, i,
            JS_NewFloat64(ctx, (double)v[i] + shift), JS_PROP_C_W_E);
    return arr;
}

// Transcript word list for pms.words fallback: AppState::words_cache (source
// seconds of state.audio_path) with lyrics_edits applied. Edit keys are
// int(word.start * fps) in source seconds, matching apply_lyrics_edits.
static std::vector<WordEntry> transcript_words_for(const AppState& state) {
    std::vector<WordEntry> out;
    out.reserve(state.words_cache.size());
    for (const WordEntry& we : state.words_cache) {
        WordEntry e = we;
        int frame = (int)(we.start * (float)state.fps);
        auto it = state.lyrics_edits.find(frame);
        if (it != state.lyrics_edits.end()) e.text = it->second;
        out.push_back(std::move(e));
    }
    return out;
}

// Timeline offset for a transcript source: same mapping as pms.audio
// (offset = in_point - start of the first Audio/Video clip whose
// text/source_id equals the source).
static double transcript_offset_for(const AppState& state, const std::string& src,
                                    bool& found) {
    found = false;
    if (src.empty()) return 0.0;
    for (auto& tr : state.tracks) {
        for (auto& cl : tr.clips) {
            if (cl.clip_type != ClipType::Audio && cl.clip_type != ClipType::Video)
                continue;
            if (cl.text == src || cl.source_id == src) {
                found = true;
                return (double)cl.in_point - (double)cl.start;
            }
        }
    }
    return 0.0;
}

static JSValue words_array_for(JSContext* ctx, const std::vector<WordEntry>& list,
                               double off, int line) {
    JSValue words = JS_NewArray(ctx);
    uint32_t i = 0;
    int li = line;
    for (auto& wd : list) {
        JSValue e = JS_NewObject(ctx);
        JS_DefinePropertyValueStr(ctx, e, "w", JS_NewString(ctx, wd.text.c_str()), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, e, "line", JS_NewInt32(ctx, li), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, e, "i", JS_NewInt32(ctx, (int)i), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, e, "t0", JS_NewFloat64(ctx, (double)wd.start - off), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, e, "t1", JS_NewFloat64(ctx, (double)wd.end - off), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, e, "conf", JS_NewFloat64(ctx, 1.0), JS_PROP_C_W_E);
        JS_DefinePropertyValueUint32(ctx, words, i++, e, JS_PROP_C_W_E);
    }
    JS_FreezeObject(ctx, words);
    return words;
}

// (Re)build memoised audio/words/lines when epoch or offset key changed.
// pms.words falls back to the project transcript when pms.audio has no words:
// words_cache (source seconds) with lyrics_edits applied, mapped to timeline
// seconds through the timeline clip that plays that source. Identity-stable:
// rebuilt only when the words, edits or mapping change.
static void ensure_audio_memo(ScriptRuntime::Impl* self) {
    JSContext* ctx = self->ctx;
    const AppState& state = *self->state;
    uint64_t epoch = script_audio_epoch(state);
    std::string key;
    const AudioAnalysis* a = state.audio_analysis.get();
    if (a) {
        bool found = false;
        double off = audio_offset_for(state, *a, found);
        char kb[64];
        snprintf(kb, sizeof(kb), "%s@%.6f", a->source.c_str(), found ? off : 0.0);
        key = kb;
        self->last_audio_off = found ? off : 0.0;
    }
    bool use_transcript = (!a || a->words.empty()) && !state.words_cache.empty() &&
                          !state.audio_path.empty();
    std::string tkey;
    double toff = 0.0;
    if (use_transcript) {
        bool found = false;
        toff = transcript_offset_for(state, state.audio_path, found);
        char kb[128];
        snprintf(kb, sizeof(kb), "%s@%.6f", state.audio_path.c_str(), toff);
        tkey = kb;
    }
    uint64_t wn = state.words_cache.size();
    double wfirst = wn ? (double)state.words_cache.front().start : 0.0;
    double wlast = wn ? (double)state.words_cache.back().end : 0.0;
    size_t en = state.lyrics_edits.size();
    const std::string& wsrc = state.audio_path;
    if (self->memo_valid && epoch == self->memo_audio_epoch && key == self->memo_audio_key &&
        use_transcript == !self->memo_trans_key.empty() &&
        (!use_transcript || (tkey == self->memo_trans_key && wn == self->memo_words_n &&
                             wfirst == self->memo_words_first && wlast == self->memo_words_last &&
                             en == self->memo_edits_n && wsrc == self->memo_words_src)))
        return;
    // Release old memoised values.
    for (JSValue* vp : {&self->memo_audio, &self->memo_words, &self->memo_lines}) {
        if (!JS_IsUndefined(*vp)) JS_FreeValue(ctx, *vp);
        *vp = JS_UNDEFINED;
    }
    self->memo_valid = true;
    self->memo_audio_epoch = epoch;
    self->memo_audio_key = key;
    self->memo_trans_key = use_transcript ? tkey : std::string();
    self->memo_trans_off = toff;
    self->memo_words_n = wn;
    self->memo_words_first = wfirst;
    self->memo_words_last = wlast;
    self->memo_edits_n = en;
    self->memo_words_src = wsrc;
    if (!a) {
        self->memo_audio = JS_NULL;
        if (use_transcript) {
            auto tw = transcript_words_for(state);
            self->memo_words = words_array_for(ctx, tw, toff, 0);
            JSValue lines = JS_NewArray(ctx);
            JS_FreezeObject(ctx, lines);
            self->memo_lines = lines;
        } else {
            self->memo_words = JS_NewArray(ctx);
            self->memo_lines = JS_NewArray(ctx);
            JS_FreezeObject(ctx, self->memo_words);
            JS_FreezeObject(ctx, self->memo_lines);
        }
        return;
    }
    double off = self->last_audio_off;
    JSValue o = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx, o, "version", JS_NewInt32(ctx, a->version), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "source", JS_NewString(ctx, a->source.c_str()), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "duration", JS_NewFloat64(ctx, a->duration), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "offset", JS_NewFloat64(ctx, off), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "bpm", JS_NewFloat64(ctx, a->bpm), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "fps", JS_NewInt32(ctx, a->fps), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "beats", new_float_array(ctx, a->beats, -off), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "downbeats", new_float_array(ctx, a->downbeats, -off), JS_PROP_C_W_E);
    JSValue hits = JS_NewObject(ctx);
    for (int k = 0; k < (int)HitKind::Count; ++k) {
        JSValue arr = JS_NewArray(ctx);
        uint32_t i = 0;
        for (auto& h : a->hits[k]) {
            JSValue e = JS_NewObject(ctx);
            JS_DefinePropertyValueStr(ctx, e, "t", JS_NewFloat64(ctx, (double)h.t - off), JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, e, "s", JS_NewFloat64(ctx, (double)h.s), JS_PROP_C_W_E);
            JS_DefinePropertyValueUint32(ctx, arr, i++, e, JS_PROP_C_W_E);
        }
        JS_DefinePropertyValueStr(ctx, hits, hit_kind_name((HitKind)k), arr, JS_PROP_C_W_E);
    }
    JS_DefinePropertyValueStr(ctx, o, "hits", hits, JS_PROP_C_W_E);
    int fps = a->fps > 0 ? a->fps : 60;
    int drop = (int)std::round(off * fps);
    JSValue env = JS_NewObject(ctx);
    for (int k = 0; k < (int)EnvKind::Count; ++k) {
        const auto& v = a->env[k];
        JSValue arr = JS_NewArray(ctx);
        uint32_t i = 0;
        if (drop < 0) {
            for (int z = 0; z < -drop; ++z)
                JS_DefinePropertyValueUint32(ctx, arr, i++, JS_NewFloat64(ctx, 0.0), JS_PROP_C_W_E);
            for (float x : v)
                JS_DefinePropertyValueUint32(ctx, arr, i++, JS_NewFloat64(ctx, x), JS_PROP_C_W_E);
        } else {
            for (size_t fi = (size_t)drop; fi < v.size(); ++fi)
                JS_DefinePropertyValueUint32(ctx, arr, i++, JS_NewFloat64(ctx, v[fi]), JS_PROP_C_W_E);
        }
        JS_DefinePropertyValueStr(ctx, env, env_kind_name((EnvKind)k), arr, JS_PROP_C_W_E);
    }
    JS_DefinePropertyValueStr(ctx, o, "env", env, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "spectrum_bands", JS_NewInt32(ctx, a->spectrum_bands), JS_PROP_C_W_E);
    {
        JSValue arr = JS_NewArray(ctx);
        uint32_t i = 0;
        int bands = a->spectrum_bands > 0 ? a->spectrum_bands : 32;
        size_t nframes = bands ? a->spectrum.size() / (size_t)bands : 0;
        auto push_frame = [&](size_t fi) {
            JSValue row = JS_NewArray(ctx);
            for (int b = 0; b < bands; ++b)
                JS_DefinePropertyValueUint32(ctx, row, (uint32_t)b,
                    JS_NewInt32(ctx, a->spectrum[fi * (size_t)bands + b]), JS_PROP_C_W_E);
            JS_DefinePropertyValueUint32(ctx, arr, i++, row, JS_PROP_C_W_E);
        };
        auto push_zero = [&]() {
            JSValue row = JS_NewArray(ctx);
            for (int b = 0; b < bands; ++b)
                JS_DefinePropertyValueUint32(ctx, row, (uint32_t)b, JS_NewInt32(ctx, 0), JS_PROP_C_W_E);
            JS_DefinePropertyValueUint32(ctx, arr, i++, row, JS_PROP_C_W_E);
        };
        if (drop < 0) {
            for (int z = 0; z < -drop; ++z) push_zero();
            for (size_t fi = 0; fi < nframes; ++fi) push_frame(fi);
        } else {
            for (size_t fi = (size_t)drop; fi < nframes; ++fi) push_frame(fi);
        }
        JS_DefinePropertyValueStr(ctx, o, "spectrum", arr, JS_PROP_C_W_E);
    }
    JSValue lines = JS_NewArray(ctx);
    { uint32_t i = 0; for (auto& l : a->lines)
        JS_DefinePropertyValueUint32(ctx, lines, i++, JS_NewString(ctx, l.c_str()), JS_PROP_C_W_E); }
    JS_FreezeObject(ctx, lines);
    JSValue words = JS_NewArray(ctx);
    if (use_transcript) {
        if (!JS_IsUndefined(words)) JS_FreeValue(ctx, words);
        auto tw = transcript_words_for(state);
        words = words_array_for(ctx, tw, toff, 0);
    } else { uint32_t i = 0; for (auto& wd : a->words) {
        JSValue e = JS_NewObject(ctx);
        JS_DefinePropertyValueStr(ctx, e, "w", JS_NewString(ctx, wd.w.c_str()), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, e, "line", JS_NewInt32(ctx, wd.line), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, e, "i", JS_NewInt32(ctx, wd.i), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, e, "t0", JS_NewFloat64(ctx, (double)wd.t0 - off), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, e, "t1", JS_NewFloat64(ctx, (double)wd.t1 - off), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, e, "conf", JS_NewFloat64(ctx, (double)wd.conf), JS_PROP_C_W_E);
        JS_DefinePropertyValueUint32(ctx, words, i++, e, JS_PROP_C_W_E);
    } }
    JS_FreezeObject(ctx, words);
    JS_DefinePropertyValueStr(ctx, o, "lines", JS_DupValue(ctx, lines), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "words", JS_DupValue(ctx, words), JS_PROP_C_W_E);
    JSValue stems = JS_NewObject(ctx);
    static const char* sn[] = {"drums","bass","other","vocals"};
    for (int k = 0; k < 4; ++k)
        JS_DefinePropertyValueStr(ctx, stems, sn[k],
            JS_NewString(ctx, a->stems[k].c_str()), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "stems", stems, JS_PROP_C_W_E);
    JS_FreezeObject(ctx, o);
    self->memo_audio = o;
    self->memo_words = words;
    self->memo_lines = lines;
}

// ── pms.* ────────────────────────────────────────────────────────────────

static JSValue pms_fn_hash(JSContext* ctx, JSValueConst thiz, int argc, JSValueConst* argv) {
    int32_t a=0,b=0,c=0;
    if (argc>0) JS_ToInt32(ctx,&a,argv[0]);
    if (argc>1) JS_ToInt32(ctx,&b,argv[1]);
    if (argc>2) JS_ToInt32(ctx,&c,argv[2]);
    int32_t h = (int32_t)((uint32_t)(a*0x27d4eb2d) ^ (uint32_t)(b*0x165667b1) ^ (uint32_t)(c*0x9e3779b1));
    auto mul = [](int32_t x, int32_t y){ return (int32_t)((uint32_t)x*(uint32_t)y); };
    h = mul(h ^ (int32_t)((uint32_t)h >> 15), (int32_t)0x85ebca6b);
    h = mul(h ^ (int32_t)((uint32_t)h >> 13), (int32_t)0xc2b2ae35);
    h ^= (int32_t)((uint32_t)h >> 16);
    return JS_NewFloat64(ctx, (double)(uint32_t)h / 4294967296.0);
}

static JSValue pms_fn_log(JSContext* ctx, JSValueConst thiz, int argc, JSValueConst* argv) {
    ScriptRuntime::Impl* self = self_of(ctx);
    std::string line;
    for (int i = 0; i < argc; ++i) {
        if (i) line += " ";
        const char* s = JS_ToCString(ctx, argv[i]);
        if (s) { line += s; JS_FreeCString(ctx, s); }
        else line += "undefined";
    }
    if (self) {
        self->log_tail.push_back(line);
        if (self->log_tail.size() > 200) self->log_tail.erase(self->log_tail.begin());
    }
    return JS_UNDEFINED;
}

static JSValue pms_fn_json(JSContext* ctx, JSValueConst thiz, int argc, JSValueConst* argv) {
    ScriptRuntime::Impl* self = self_of(ctx);
    if (!self || argc < 1 || !JS_IsString(argv[0])) {
        JS_ThrowTypeError(ctx, "pms.json needs a path string");
        return JS_EXCEPTION;
    }
    const char* sp = JS_ToCString(ctx, argv[0]);
    std::string spec = sp ? sp : "";
    if (sp) JS_FreeCString(ctx, sp);
    std::string resolved = script_resolve_spec(spec, self->entry_path, self->entry_dir);
    if (resolved.empty()) {
        // Resolve against the script file dir (entry-relative already tried);
        // fall back to CWD-relative.
        resolved = ((fs::path(self->entry_dir) / spec).string());
    }
    uint64_t mt = file_mtime_ns(resolved);
    if (mt == 0) {
        JS_ThrowTypeError(ctx, "pms.json: cannot read '%s'", spec.c_str());
        return JS_EXCEPTION;
    }
    auto it = self->json_cache.find(resolved);
    if (it != self->json_cache.end() && it->second.mtime == mt)
        return JS_DupValue(ctx, it->second.value);
    std::string src;
    if (!read_file(resolved, src)) {
        JS_ThrowTypeError(ctx, "pms.json: cannot read '%s'", spec.c_str());
        return JS_EXCEPTION;
    }
    JSValue v = JS_ParseJSON(ctx, src.c_str(), src.size(), resolved.c_str());
    if (JS_IsException(v)) return JS_EXCEPTION;
    JS_FreezeObject(ctx, v);
    auto& e = self->json_cache[resolved];
    if (!JS_IsUndefined(e.value)) JS_FreeValue(ctx, e.value);
    e.mtime = mt;
    e.value = JS_DupValue(ctx, v);
    self->watched[resolved] = mt;
    return v;
}

static JSValue pms_fn_image(JSContext* ctx, JSValueConst thiz, int argc, JSValueConst* argv) {
    ScriptRuntime::Impl* self = self_of(ctx);
    if (!self || argc < 1 || !JS_IsString(argv[0])) {
        JS_ThrowTypeError(ctx, "pms.image needs a path string");
        return JS_EXCEPTION;
    }
    const char* sp = JS_ToCString(ctx, argv[0]);
    std::string spec = sp ? sp : "";
    if (sp) JS_FreeCString(ctx, sp);
    std::string resolved = script_resolve_spec(spec, self->entry_path, self->entry_dir);
    if (resolved.empty() || file_mtime_ns(resolved) == 0)
        resolved = (fs::path(self->entry_dir) / spec).string();
    auto it = self->image_cache.find(resolved);
    if (it != self->image_cache.end()) return JS_DupValue(ctx, it->second);
    JSValue o = c2d_new_image(ctx, resolved);
    if (JS_IsException(o)) return o;
    self->image_cache[resolved] = JS_DupValue(ctx, o);
    self->watched[resolved] = file_mtime_ns(resolved);
    return o;
}

static JSValue pms_fn_face(JSContext* ctx, JSValueConst thiz, int argc, JSValueConst* argv) {
    ScriptRuntime::Impl* self = self_of(ctx);
    if (!self || argc < 1 || !JS_IsString(argv[0])) {
        JS_ThrowTypeError(ctx, "pms.face needs a path string");
        return JS_EXCEPTION;
    }
    const char* sp = JS_ToCString(ctx, argv[0]);
    std::string spec = sp ? sp : "";
    if (sp) JS_FreeCString(ctx, sp);
    std::string resolved = script_resolve_spec(spec, self->entry_path, self->entry_dir);
    if (resolved.empty() || !fs::exists(resolved))
        resolved = (fs::path(self->entry_dir) / spec).string();
    bool known = false;
    for (auto& r : self->face_requests) if (r == resolved) { known = true; break; }
    if (!known) self->face_requests.push_back(resolved);
    // rot_q 0 default (pms.face media is expected upright; rotation comes
    // from the clip transform, not the file).
    face_cache_request(resolved, 0);
    float prog = 0.f;
    FaceCacheStatus st = face_cache_status(resolved, &prog);
    if (st != FaceCacheStatus::Ready) return JS_NULL;  // null while building
    auto it = self->face_cache.find(resolved);
    if (it != self->face_cache.end()) return JS_DupValue(ctx, it->second);
    std::string dump;
    std::string err;
    if (!face_track_dump_json(resolved, 0, dump, &err)) return JS_NULL;
    JSValue v = JS_ParseJSON(ctx, dump.c_str(), dump.size(), "<face>");
    if (JS_IsException(v)) return JS_EXCEPTION;
    JS_FreezeObject(ctx, v);
    self->face_cache[resolved] = JS_DupValue(ctx, v);
    return v;
}

static JSValue pms_fn_font(JSContext* ctx, JSValueConst thiz, int argc, JSValueConst* argv) {
    ScriptRuntime::Impl* self = self_of(ctx);
    if (!self || argc < 2 || !JS_IsString(argv[0]) || !JS_IsString(argv[1])) {
        JS_ThrowTypeError(ctx, "pms.font needs (family, path)");
        return JS_EXCEPTION;
    }
    const char* fam = JS_ToCString(ctx, argv[0]);
    const char* sp = JS_ToCString(ctx, argv[1]);
    std::string family = fam ? fam : "", spec = sp ? sp : "";
    if (fam) JS_FreeCString(ctx, fam);
    if (sp) JS_FreeCString(ctx, sp);
    std::string resolved = script_resolve_spec(spec, self->entry_path, self->entry_dir);
    if (resolved.empty() || file_mtime_ns(resolved) == 0)
        resolved = (fs::path(self->entry_dir) / spec).string();
    std::string err;
    if (!c2d_register_font(ctx, self->canvas_obj, family, resolved, &err)) {
        JS_ThrowTypeError(ctx, "pms.font: %s", err.c_str());
        return JS_EXCEPTION;
    }
    self->watched[resolved] = file_mtime_ns(resolved);
    return JS_UNDEFINED;
}

static JSValue pms_fn_post(JSContext* ctx, JSValueConst thiz, int argc, JSValueConst* argv) {
    ScriptRuntime::Impl* self = self_of(ctx);
    if (!self || argc < 1 || !JS_IsString(argv[0])) {
        JS_ThrowTypeError(ctx, "pms.post needs (frag, uniforms?)");
        return JS_EXCEPTION;
    }
    const char* fp = JS_ToCString(ctx, argv[0]);
    self->post_frag = fp ? fp : "";
    if (fp) JS_FreeCString(ctx, fp);
    self->post_uniforms.clear();
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSPropertyEnum* tab = nullptr;
        uint32_t len = 0;
        if (JS_GetOwnPropertyNames(ctx, &tab, &len, argv[1],
                JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
            for (uint32_t i = 0; i < len; ++i) {
                const char* k = JS_AtomToCString(ctx, tab[i].atom);
                JSValue v = JS_GetProperty(ctx, argv[1], tab[i].atom);
                std::vector<float> vec;
                if (JS_IsNumber(v)) {
                    double d = 0; JS_ToFloat64(ctx, &d, v);
                    vec.push_back((float)d);
                } else if (JS_IsObject(v)) {
                    JSValue l = JS_GetPropertyStr(ctx, v, "length");
                    int32_t n = 0;
                    if (!JS_IsException(l)) JS_ToInt32(ctx, &n, l);
                    JS_FreeValue(ctx, l);
                    for (int32_t j = 0; j < n && j < 4; ++j) {
                        JSValue e = JS_GetPropertyUint32(ctx, v, (uint32_t)j);
                        double d = 0; JS_ToFloat64(ctx, &d, e);
                        vec.push_back((float)d);
                        JS_FreeValue(ctx, e);
                    }
                }
                JS_FreeValue(ctx, v);
                if (k) {
                    if (!vec.empty() && vec.size() <= 4)
                        self->post_uniforms.push_back({k, vec});
                    JS_FreeCString(ctx, k);
                }
            }
            JS_FreePropertyEnum(ctx, tab, len);
        }
    }
    return JS_UNDEFINED;
}

// Purity stubs: throw on access/call.
static JSValue purity_throw(JSContext* ctx, JSValueConst thiz, int argc, JSValueConst* argv) {
    (void)thiz; (void)argc; (void)argv;
    JS_ThrowTypeError(ctx, "non-deterministic API is disabled in render(); use pms.hash");
    return JS_EXCEPTION;
}

static void install_purity_stubs(JSContext* ctx, JSValue global) {
    // Math.random → throw.
    JSValue math = JS_GetPropertyStr(ctx, global, "Math");
    if (!JS_IsException(math) && !JS_IsUndefined(math)) {
        JSValue fn = JS_NewCFunction(ctx, purity_throw, "random", 0);
        JS_DefinePropertyValueStr(ctx, math, "random", fn, JS_PROP_C_W_E);
    }
    JS_FreeValue(ctx, math);
    // Date.now → throw (keep constructor for parsing).
    JSValue date = JS_GetPropertyStr(ctx, global, "Date");
    if (!JS_IsException(date) && !JS_IsUndefined(date)) {
        JSValue fn = JS_NewCFunction(ctx, purity_throw, "now", 0);
        JS_DefinePropertyValueStr(ctx, date, "now", fn, JS_PROP_C_W_E);
    }
    JS_FreeValue(ctx, date);
    // Timers / rAF.
    const char* names[] = {"setTimeout","clearTimeout","setInterval","clearInterval",
                           "requestAnimationFrame","cancelAnimationFrame", nullptr};
    for (int i = 0; names[i]; ++i) {
        JSValue fn = JS_NewCFunction(ctx, purity_throw, names[i], 0);
        JS_DefinePropertyValueStr(ctx, global, names[i], fn, JS_PROP_C_W_E);
    }
    // console.log → script log.
    JSValue con = JS_NewObject(ctx);
    JSValue logfn = JS_NewCFunction(ctx, pms_fn_log, "log", 0);
    JS_DefinePropertyValueStr(ctx, con, "log", logfn, JS_PROP_C_W_E);
    JSValue log2 = JS_NewCFunction(ctx, pms_fn_log, "warn", 0);
    JS_DefinePropertyValueStr(ctx, con, "warn", log2, JS_PROP_C_W_E);
    JSValue log3 = JS_NewCFunction(ctx, pms_fn_log, "error", 0);
    JS_DefinePropertyValueStr(ctx, con, "error", log3, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, global, "console", con, JS_PROP_C_W_E);
}

// ── Build / render ─────────────────────────────────────────────────────

// First "file:line" of the exception stack ("at fn (file:line:col)").
static void locate_error(const std::string& text, ScriptError& e) {
    size_t at = text.find("    at ");
    while (at != std::string::npos) {
        size_t eol = text.find('\n', at);
        std::string frame = text.substr(at + 7, eol == std::string::npos ? std::string::npos
                                                                          : eol - at - 7);
        size_t open = frame.find('(');
        std::string loc = open != std::string::npos
            ? frame.substr(open + 1, frame.find(')', open) - open - 1) : frame;
        size_t c2 = loc.rfind(':');
        size_t c1 = c2 == std::string::npos || c2 == 0 ? std::string::npos : loc.rfind(':', c2 - 1);
        if (c1 != std::string::npos && loc.compare(0, 1, "<") != 0) {
            e.file = loc.substr(0, c1);
            e.line = std::atoi(loc.substr(c1 + 1, c2 - c1 - 1).c_str());
            return;
        }
        at = eol == std::string::npos ? std::string::npos : text.find("    at ", eol);
    }
}

static ScriptError exception_error(JSContext* ctx, const std::string& fallback_file) {
    ScriptError e;
    e.message = js_exc_text(ctx);
    e.file = fallback_file;
    locate_error(e.message, e);
    return e;
}

void ScriptRuntime::teardown() {
    Impl* self = impl_.get();
    if (!self->rt) return;
    JSContext* ctx = self->ctx;
    for (auto& kv : self->json_cache) JS_FreeValue(ctx, kv.second.value);
    for (auto& kv : self->face_cache) JS_FreeValue(ctx, kv.second);
    for (auto& kv : self->image_cache) JS_FreeValue(ctx, kv.second);
    self->json_cache.clear();
    self->face_cache.clear();
    self->image_cache.clear();
    for (JSValue* v : {&self->memo_audio, &self->memo_words, &self->memo_lines,
                       &self->canvas_obj, &self->pms_obj, &self->setup_fn,
                       &self->render_fn, &self->entry_ns}) {
        JS_FreeValue(ctx, *v);
        *v = JS_UNDEFINED;
    }
    self->memo_valid = false;
    JS_FreeContext(ctx);
    JS_FreeRuntime(self->rt);
    self->rt = nullptr;
    self->ctx = nullptr;
}

// Getter-only accessor on `obj` whose value is one of the memoised audio
// objects (identity-stable between rebuilds of the memo).
static void define_audio_getter(JSContext* ctx, JSValueConst obj, const char* name, int which) {
    JSValue get = JS_NewCFunctionMagic(ctx,
        [](JSContext* c, JSValueConst, int, JSValueConst*, int magic) -> JSValue {
            ScriptRuntime::Impl* s = self_of(c);
            ensure_audio_memo(s);
            JSValue v = magic == 0 ? s->memo_audio : magic == 1 ? s->memo_words : s->memo_lines;
            return JS_DupValue(c, v);
        }, name, 0, JS_CFUNC_generic_magic, which);
    JSAtom atom = JS_NewAtom(ctx, name);
    JS_DefinePropertyGetSet(ctx, obj, atom, get, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, atom);
}

bool ScriptRuntime::build(const AppState& state, const Clip& clip, int width, int height,
                          std::vector<ScriptError>& errors) {
    Impl* self = impl_.get();
    teardown();
    self->watched.clear();
    self->log_tail.clear();
    self->face_requests.clear();
    self->post_frag.clear();
    self->post_uniforms.clear();
    self->state = &state;

    // Entry: absolute, or relative to the project file.
    std::string entry = clip.script_path;
    if (!entry.empty() && entry[0] != '/') {
        std::string base = fs::path(state.project_path).parent_path().string();
        if (!base.empty()) entry = (fs::path(base) / entry).string();
    }
    std::error_code ec;
    fs::path centry = entry.empty() ? fs::path() : fs::canonical(entry, ec);
    if (entry.empty() || ec) {
        errors.push_back(ScriptError{"script file not found: " + clip.script_path, clip.script_path, 0});
        return false;
    }
    self->entry_path = centry.string();
    self->entry_dir = centry.parent_path().string();

    self->rt = JS_NewRuntime();
    if (!self->rt) {
        errors.push_back(ScriptError{"cannot create JS runtime", "", 0});
        return false;
    }
    JS_SetMemoryLimit(self->rt, (size_t)512 * 1024 * 1024);
    JS_SetMaxStackSize(self->rt, (size_t)4 * 1024 * 1024);
    self->ctx = JS_NewContext(self->rt);
    if (!self->ctx) {
        JS_FreeRuntime(self->rt);
        self->rt = nullptr;
        errors.push_back(ScriptError{"cannot create JS context", "", 0});
        return false;
    }
    JSContext* ctx = self->ctx;
    JS_SetContextOpaque(ctx, self);
    JS_SetModuleLoaderFunc(self->rt, script_normalize, script_loader, self);

    JSValue global = JS_GetGlobalObject(ctx);
    install_purity_stubs(ctx, global);
    self->pms_obj = JS_NewObject(ctx);
    self->canvas_obj = c2d_new_context(ctx);
    JS_DefinePropertyValueStr(ctx, self->pms_obj, "canvas", JS_DupValue(ctx, self->canvas_obj),
                              JS_PROP_ENUMERABLE);
    {
        const char* ps = clip.script_params.empty() ? "{}" : clip.script_params.c_str();
        JSValue v = JS_ParseJSON(ctx, ps, strlen(ps), "<script_params>");
        if (JS_IsException(v)) {
            errors.push_back(exception_error(ctx, "<script_params>"));
            JS_FreeValue(ctx, global);
            return false;
        }
        JS_FreezeObject(ctx, v);
        JS_DefinePropertyValueStr(ctx, self->pms_obj, "params", v, JS_PROP_ENUMERABLE);
    }
    define_audio_getter(ctx, self->pms_obj, "audio", 0);
    define_audio_getter(ctx, self->pms_obj, "words", 1);
    define_audio_getter(ctx, self->pms_obj, "lines", 2);
    static const struct { const char* name; JSCFunction* fn; int len; } kFns[] = {
        {"json", pms_fn_json, 1}, {"image", pms_fn_image, 1}, {"face", pms_fn_face, 1},
        {"font", pms_fn_font, 2}, {"post", pms_fn_post, 2}, {"hash", pms_fn_hash, 3},
        {"log", pms_fn_log, 0},
    };
    for (const auto& f : kFns)
        JS_DefinePropertyValueStr(ctx, self->pms_obj, f.name,
                                  JS_NewCFunction(ctx, f.fn, f.name, f.len), JS_PROP_ENUMERABLE);
    JS_FreezeObject(ctx, self->pms_obj);
    JS_DefinePropertyValueStr(ctx, global, "pms", JS_DupValue(ctx, self->pms_obj),
                              JS_PROP_ENUMERABLE);
    JS_FreeValue(ctx, global);

    // Compile + link + run the entry module.
    std::string src;
    if (!read_file(self->entry_path, src)) {
        errors.push_back(ScriptError{"cannot read " + self->entry_path, self->entry_path, 0});
        return false;
    }
    self->watched[self->entry_path] = file_mtime_ns(self->entry_path);
    JSValue func = JS_Eval(ctx, src.c_str(), src.size(), self->entry_path.c_str(),
                           JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(func)) {
        errors.push_back(exception_error(ctx, self->entry_path));
        return false;
    }
    if (JS_ResolveModule(ctx, func) < 0) {
        JS_FreeValue(ctx, func);
        errors.push_back(exception_error(ctx, self->entry_path));
        return false;
    }
    JSModuleDef* m = (JSModuleDef*)JS_VALUE_GET_PTR(func);
    JSValue run = JS_EvalFunction(ctx, func);
    if (JS_IsException(run)) {
        errors.push_back(exception_error(ctx, self->entry_path));
        return false;
    }
    // Module evaluation returns a promise; settle it, a rejection carries
    // the error.
    {
        JSContext* jc = nullptr;
        while (JS_ExecutePendingJob(self->rt, &jc) > 0) {}
    }
    if (JS_PromiseState(ctx, run) == JS_PROMISE_REJECTED) {
        JS_Throw(ctx, JS_PromiseResult(ctx, run));
        JS_FreeValue(ctx, run);
        errors.push_back(exception_error(ctx, self->entry_path));
        return false;
    }
    JS_FreeValue(ctx, run);
    self->entry_ns = JS_GetModuleNamespace(ctx, m);
    self->render_fn = JS_GetPropertyStr(ctx, self->entry_ns, "render");
    if (!JS_IsFunction(ctx, self->render_fn)) {
        errors.push_back(ScriptError{"the entry module must export function render(f)",
                                     self->entry_path, 0});
        return false;
    }
    self->setup_fn = JS_GetPropertyStr(ctx, self->entry_ns, "setup");
    if (JS_IsUndefined(self->setup_fn)) return true;
    if (!JS_IsFunction(ctx, self->setup_fn)) {
        errors.push_back(ScriptError{"export setup must be a function", self->entry_path, 0});
        return false;
    }
    JSValue env = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx, env, "width", JS_NewInt32(ctx, width), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, env, "height", JS_NewInt32(ctx, height), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, env, "fps", JS_NewFloat64(ctx, state.fps), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, env, "params", JS_GetPropertyStr(ctx, self->pms_obj, "params"),
                              JS_PROP_C_W_E);
    JS_FreezeObject(ctx, env);
    JSValue r = JS_Call(ctx, self->setup_fn, self->entry_ns, 1, &env);
    JS_FreeValue(ctx, env);
    if (JS_IsException(r)) {
        errors.push_back(exception_error(ctx, self->entry_path));
        return false;
    }
    JS_FreeValue(ctx, r);
    return true;
}

bool ScriptRuntime::render(const AppState& state, const ScriptFrame& f, SkCanvas* canvas,
                           std::vector<ScriptError>& errors) {
    Impl* self = impl_.get();
    if (!self->ctx) return false;
    JSContext* ctx = self->ctx;
    self->state = &state;
    self->post_frag.clear();
    self->post_uniforms.clear();
    self->face_requests.clear();
    ensure_audio_memo(self);
    JSValue fo = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx, fo, "t", JS_NewFloat64(ctx, f.t), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, fo, "local", JS_NewFloat64(ctx, f.local), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, fo, "frame", JS_NewInt32(ctx, f.frame), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, fo, "fps", JS_NewFloat64(ctx, f.fps), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, fo, "width", JS_NewInt32(ctx, f.width), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, fo, "height", JS_NewInt32(ctx, f.height), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, fo, "duration", JS_NewFloat64(ctx, f.duration), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, fo, "exporting", JS_NewBool(ctx, f.exporting), JS_PROP_C_W_E);
    JS_FreezeObject(ctx, fo);
    c2d_begin_frame(ctx, self->canvas_obj, canvas, f.width, f.height);
    JSValue r = JS_Call(ctx, self->render_fn, self->entry_ns, 1, &fo);
    c2d_begin_frame(ctx, self->canvas_obj, nullptr, f.width, f.height);
    JS_FreeValue(ctx, fo);
    bool ok = true;
    if (JS_IsException(r)) {
        errors.push_back(exception_error(ctx, self->entry_path));
        ok = false;
    }
    JS_FreeValue(ctx, r);
    // render() is synchronous: drain anything it queued, then report it.
    if (JS_IsJobPending(self->rt)) {
        JSContext* jc = nullptr;
        while (JS_ExecutePendingJob(self->rt, &jc) > 0) {}
        errors.push_back(ScriptError{"render(f) must be synchronous (it queued a promise job)",
                                     self->entry_path, 0});
        ok = false;
    }
    return ok;
}

bool ScriptRuntime::poll_dirty() const {
    for (const auto& kv : impl_->watched)
        if (file_mtime_ns(kv.first) != kv.second) return true;
    return false;
}

const std::vector<std::string>& ScriptRuntime::log_tail() const { return impl_->log_tail; }
const std::string& ScriptRuntime::post_frag() const { return impl_->post_frag; }
const ScriptUniforms& ScriptRuntime::post_uniforms() const { return impl_->post_uniforms; }
const std::vector<std::string>& ScriptRuntime::face_requests() const {
    return impl_->face_requests;
}
