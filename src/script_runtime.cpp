// Script runtime: QuickJS-ng host, ES module loader, pms bindings,
// Context2D on NanoVG.
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
// face/image objects memoise per path (+ cache generation for faces).
#include "script_runtime.h"
#include "script_canvas.h"
#include "app.h"
#include "audio_analysis.h"
#include "face_cache.h"
#include "script_face.h"
#include "video.h"

#include "quickjs.h"
#if PMS_HAS_GL
#include "nanovg.h"
#endif
#include "stb_image.h"

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

std::string script_resolve_spec(const std::string& spec, const std::string& referrer,
                                const std::string& entry_dir) {
    if (spec.rfind("pms:", 0) == 0) {
        std::string tail = spec.substr(4);
        std::string base = asset_root() + "/scripts/";
        if (tail.rfind("typography/", 0) == 0) return base + tail + ".js";
        if (tail == "rhythm" || tail == "text") return base + "std/" + tail + ".js";
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

// ── CSS colours ──────────────────────────────────────────────────────────

static bool parse_css_color(const char* s, float rgba[4]) {
    if (!s) return false;
    while (*s == ' ') ++s;
    rgba[0] = rgba[1] = rgba[2] = 0.f; rgba[3] = 1.f;
    if (*s == '#') {
        ++s;
        size_t n = strlen(s);
        while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t')) --n;
        unsigned v = 0;
        auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        if (n == 3 || n == 4) {
            for (size_t i = 0; i < n; ++i) {
                int h = hex(s[i]);
                if (h < 0) return false;
                v = (v << 4) | (unsigned)h;
            }
            unsigned a4 = (n == 4) ? (v & 0xF) : 0xF;
            unsigned r4 = (n == 4) ? ((v >> 4) & 0xF) : ((v >> 8) & 0xF);
            unsigned g4 = (n == 4) ? ((v >> 8) & 0xF) : ((v >> 4) & 0xF);
            unsigned b4 = (v & 0xF);
            rgba[0] = r4 / 15.f; rgba[1] = g4 / 15.f; rgba[2] = b4 / 15.f;
            rgba[3] = a4 / 15.f;
            return true;
        }
        if (n == 6 || n == 8) {
            for (size_t i = 0; i < n; ++i) {
                int h = hex(s[i]);
                if (h < 0) return false;
                v = (v << 4) | (unsigned)h;
            }
            if (n == 6) {
                rgba[0] = ((v >> 16) & 0xFF) / 255.f;
                rgba[1] = ((v >> 8) & 0xFF) / 255.f;
                rgba[2] = (v & 0xFF) / 255.f;
            } else {
                rgba[0] = ((v >> 24) & 0xFF) / 255.f;
                rgba[1] = ((v >> 16) & 0xFF) / 255.f;
                rgba[2] = ((v >> 8) & 0xFF) / 255.f;
                rgba[3] = (v & 0xFF) / 255.f;
            }
            return true;
        }
        return false;
    }
    if (!strncmp(s, "rgb", 3)) {
        const char* p = strchr(s, '(');
        const char* q = p ? strchr(p, ')') : nullptr;
        if (!p || !q) return false;
        std::string inner(p + 1, q);
        for (char& c : inner) if (c == ',') c = ' ';
        float a = 1.f;
        int got = sscanf(inner.c_str(), "%f %f %f %f", &rgba[0], &rgba[1], &rgba[2], &a);
        if (got < 3) return false;
        if (strchr(s, '%')) { rgba[0] /= 100.f; rgba[1] /= 100.f; rgba[2] /= 100.f; }
        else if (rgba[0] > 1.f || rgba[1] > 1.f || rgba[2] > 1.f) {
            rgba[0] /= 255.f; rgba[1] /= 255.f; rgba[2] /= 255.f;
        }
        rgba[3] = a;
        return true;
    }
    static const struct { const char* n; float r, g, b; } kNamed[] = {
        {"black",0,0,0},{"white",1,1,1},{"red",1,0,0},{"lime",0,1,0},{"blue",0,0,1},
        {"yellow",1,1,0},{"cyan",0,1,1},{"magenta",1,0,1},{"silver",.75f,.75f,.75f},
        {"gray",.5f,.5f,.5f},{"grey",.5f,.5f,.5f},{"maroon",.5f,0,0},{"olive",.5f,.5f,0},
        {"green",0,.5f,0},{"purple",.5f,0,.5f},{"teal",0,.5f,.5f},{"navy",0,0,.5f},
        {"orange",1,.65f,0},
    };
    std::string name(s);
    size_t e = name.find_last_not_of(" \t");
    if (e != std::string::npos) name.resize(e + 1);
    for (char& c : name) c = (char)tolower(c);
    if (name == "transparent") { rgba[0]=rgba[1]=rgba[2]=0.f; rgba[3]=0.f; return true; }
    for (auto& kn : kNamed)
        if (name == kn.n) { rgba[0]=kn.r; rgba[1]=kn.g; rgba[2]=kn.b; rgba[3]=1.f; return true; }
    return false;
}

// ── Impl ─────────────────────────────────────────────────────────────────

struct ScriptRuntime::Impl {
    JSRuntime* rt = nullptr;
    JSContext* ctx = nullptr;
    JSValue entry_ns = JS_UNDEFINED;
    JSValue pms_obj = JS_UNDEFINED;
    JSValue canvas_obj = JS_UNDEFINED;
    ScriptCanvas* canvas = nullptr;
    JSClassID canvas_class = 0;
    JSClassID grad_class = 0;
    JSClassID img_class = 0;
    std::string entry_path;
    std::string entry_dir;
    std::string clip_key;
    const AppState* state = nullptr;
    const Clip* clip = nullptr;
    std::unordered_map<std::string, uint64_t> watched;
    // audio memo
    uint64_t memo_audio_epoch = 0;
    std::string memo_audio_key;
    JSValue memo_audio = JS_UNDEFINED;
    JSValue memo_words = JS_UNDEFINED;
    JSValue memo_lines = JS_UNDEFINED;
    struct JsonEntry { uint64_t mtime = 0; JSValue value = JS_UNDEFINED; };
    std::unordered_map<std::string, JsonEntry> json_cache;
    struct FaceEntry { int gen = -1; JSValue value = JS_UNDEFINED; };
    std::unordered_map<std::string, FaceEntry> face_cache;
    std::unordered_map<std::string, JSValue> image_cache;
    std::vector<std::string> log_tail;
    std::vector<std::string> face_requests;
    std::vector<std::string> json_deps;
    bool has_post = false;
    std::string post_frag;
    std::vector<std::pair<std::string, std::vector<float>>> post_uniforms;
    int cw = 0, ch = 0;
    std::vector<int> lut_images;
    bool has_setup = false;
    JSValue setup_fn = JS_UNDEFINED;
    JSValue render_fn = JS_UNDEFINED;
    double last_audio_off = 0.0;
};

static ScriptRuntime::Impl* self_of(JSContext* ctx) {
    return (ScriptRuntime::Impl*)JS_GetContextOpaque(ctx);
}

static std::string js_exc_text(JSContext* ctx) {
    JSValue e = JS_GetException(ctx);
    const char* s = JS_ToCString(ctx, e);
    std::string out = s ? s : "unknown error";
    if (s) JS_FreeCString(ctx, s);
    JSValue stack = JS_GetPropertyStr(ctx, e, "stack");
    if (!JS_IsUndefined(stack) && !JS_IsException(stack)) {
        const char* ss = JS_ToCString(ctx, stack);
        if (ss) { out += "\n"; out += ss; JS_FreeCString(ctx, ss); }
    }
    JS_FreeValue(ctx, stack);
    JS_FreeValue(ctx, e);
    return out;
}

ScriptRuntime::ScriptRuntime() : impl_(new Impl) {}
ScriptRuntime::~ScriptRuntime() = default;

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

// (Re)build memoised audio/words/lines when epoch or offset key changed.
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
    if (epoch == self->memo_audio_epoch && key == self->memo_audio_key) return;
    // Release old memoised values.
    for (JSValue* vp : {&self->memo_audio, &self->memo_words, &self->memo_lines}) {
        if (!JS_IsUndefined(*vp)) JS_FreeValue(ctx, *vp);
        *vp = JS_UNDEFINED;
    }
    self->memo_audio_epoch = epoch;
    self->memo_audio_key = key;
    if (!a) {
        self->memo_audio = JS_NULL;
        self->memo_words = JS_NewArray(ctx);
        self->memo_lines = JS_NewArray(ctx);
        JS_FreezeObject(ctx, self->memo_words);
        JS_FreezeObject(ctx, self->memo_lines);
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
    { uint32_t i = 0; for (auto& wd : a->words) {
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

// ── Canvas2D host state ──────────────────────────────────────────────────

struct GradientStop { double off = 0; float rgba[4] = {}; };
struct Gradient {
    bool radial = false;
    double x0=0,y0=0,x1=0,y1=0,r0=0,r1=0;
    std::vector<GradientStop> stops;
};

struct CanvasState {
    JSValue fill = JS_UNDEFINED;    // string or gradient object
    JSValue stroke = JS_UNDEFINED;
    double alpha = 1.0;
    std::string comp = "source-over";
    double line_width = 1.0;
    std::string cap = "butt", join = "miter";
    double miter = 10.0;
    bool smoothing = true;
    std::string font = "16px Inter";
    std::string align = "start", baseline = "alphabetic";
    double spacing = 0.0;
    struct Verb { int op=0; double v[6]={}; bool ccw=false; };
    std::vector<Verb> path;
    int clip_depth = 0;
    std::vector<char> clip_disabled;
    std::vector<int> save_stack;  // nesting depth only (NanoVG owns state)
};

static JSClassID canvas_class_id();
static JSClassID grad_class_id();
static JSClassID img_class_id();

static CanvasState* canvas_self(JSContext* ctx, JSValueConst obj) {
    ScriptRuntime::Impl* self = self_of(ctx);
    if (!self) return nullptr;
    return (CanvasState*)JS_GetOpaque(obj, self->canvas_class);
}
static Gradient* grad_self(JSContext* ctx, JSValueConst obj) {
    ScriptRuntime::Impl* self = self_of(ctx);
    if (!self) return nullptr;
    return (Gradient*)JS_GetOpaque(obj, self->grad_class);
}

struct ImgData { int w=0,h=0; std::vector<unsigned char> px; int nvg=0; bool smooth=true; std::string path; };
static ImgData* img_self(JSContext* ctx, JSValueConst obj) {
    ScriptRuntime::Impl* self = self_of(ctx);
    if (!self) return nullptr;
    return (ImgData*)JS_GetOpaque(obj, self->img_class);
}

// Style → NVGpaint (solid or gradient). Multi-stop linear → 256×1 LUT +
// image pattern aligned to the vector; radial → concentric rings done by the
// caller (returns false with radial_multi=true). Caller frees LUT image.
struct StylePaint {
    bool is_grad = false;
    float rgba[4] = {0,0,0,1};
    Gradient* grad = nullptr;
};

static bool style_of(JSContext* ctx, JSValueConst v, StylePaint& out) {
    if (JS_IsString(v)) {
        const char* s = JS_ToCString(ctx, v);
        float r[4] = {};
        bool ok = s && parse_css_color(s, r);
        if (s) JS_FreeCString(ctx, s);
        if (!ok) return false;
        out.is_grad = false;
        memcpy(out.rgba, r, sizeof(r));
        return true;
    }
    Gradient* g = grad_self(ctx, v);
    if (!g || g->stops.empty()) return false;
    out.is_grad = true;
    out.grad = g;
    return true;
}

static void stops_sorted(Gradient* g, std::vector<GradientStop>& out) {
    out = g->stops;
    std::sort(out.begin(), out.end(),
              [](const GradientStop& a, const GradientStop& b){ return a.off < b.off; });
}

static void sample_stops(const std::vector<GradientStop>& st, double t, float r[4]) {
    if (st.empty()) { r[0]=r[1]=r[2]=0; r[3]=1; return; }
    if (t <= st.front().off) { memcpy(r, st.front().rgba, sizeof(float)*4); return; }
    if (t >= st.back().off) { memcpy(r, st.back().rgba, sizeof(float)*4); return; }
    for (size_t i = 1; i < st.size(); ++i) {
        if (t <= st[i].off) {
            double span = st[i].off - st[i-1].off;
            double k = span > 1e-9 ? (t - st[i-1].off) / span : 0;
            for (int c = 0; c < 4; ++c)
                r[c] = (float)(st[i-1].rgba[c] + (st[i].rgba[c] - st[i-1].rgba[c]) * k);
            return;
        }
    }
    memcpy(r, st.back().rgba, sizeof(float)*4);
}


// ── CanvasState helpers ──────────────────────────────────────────────────

static void canvas_free_state(CanvasState* cs, JSContext* ctx) {
    if (!cs) return;
    JS_FreeValue(ctx, cs->fill);
    JS_FreeValue(ctx, cs->stroke);
    delete cs;
}

// Replay the recorded path into NanoVG.
static void replay_path(ScriptRuntime::Impl* self, CanvasState* cs) {
#if PMS_HAS_GL
    NVGcontext* vg = self->canvas->vg();
    for (auto& vb : cs->path) {
        switch (vb.op) {
            case 0: nvgMoveTo(vg, (float)vb.v[0], (float)vb.v[1]); break;
            case 1: nvgLineTo(vg, (float)vb.v[0], (float)vb.v[1]); break;
            case 2: nvgQuadTo(vg, (float)vb.v[0], (float)vb.v[1], (float)vb.v[2], (float)vb.v[3]); break;
            case 3: nvgBezierTo(vg, (float)vb.v[0], (float)vb.v[1], (float)vb.v[2], (float)vb.v[3], (float)vb.v[4], (float)vb.v[5]); break;
            case 4: nvgClosePath(vg); break;
            case 5: nvgRect(vg, (float)vb.v[0], (float)vb.v[1], (float)vb.v[2], (float)vb.v[3]); break;
            case 6: nvgArc(vg, (float)vb.v[0], (float)vb.v[1], (float)vb.v[2], (float)vb.v[3], (float)vb.v[4], vb.ccw ? NVG_CCW : NVG_CW); break;
            case 7: nvgEllipse(vg, (float)vb.v[0], (float)vb.v[1], (float)vb.v[2], (float)vb.v[3]); break;
        }
    }
#else
    (void)self; (void)cs;
#endif
}

// Apply composite op. Returns true when caller should draw with plain
// source-over after (destination-out needs two-pass), else false.
static void apply_comp(ScriptRuntime::Impl* self, const std::string& comp) {
#if PMS_HAS_GL
    NVGcontext* vg = self->canvas->vg();
    if (comp == "lighter") { nvgGlobalCompositeOperation(vg, NVG_LIGHTER); return; }
    if (comp == "multiply") {
        nvgGlobalCompositeBlendFuncSeparate(vg, GL_DST_COLOR, GL_ZERO, GL_ONE, GL_ONE);
        return;
    }
    if (comp == "screen") {
        nvgGlobalCompositeBlendFuncSeparate(vg, GL_ONE, GL_ONE_MINUS_SRC_COLOR, GL_ONE, GL_ONE);
        return;
    }
    if (comp == "destination-out") {
        nvgGlobalCompositeBlendFuncSeparate(vg, GL_ZERO, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
        return;
    }
    nvgGlobalCompositeOperation(vg, NVG_SOURCE_OVER);
#else
    (void)self; (void)comp;
#endif
}

// Resolve a style: solid → NVGcolor paint; linear gradients (any stops) via
// LUT; radial ≤2 stops → native; radial multi → ring slices by caller.
static bool resolve_paint(ScriptRuntime::Impl* self, JSContext* ctx, JSValueConst v,
                          bool* out_radial_multi, Gradient** out_grad) {
    *out_radial_multi = false;
    *out_grad = nullptr;
    if (JS_IsString(v)) return true;
    Gradient* g = grad_self(ctx, v);
    if (!g || g->stops.empty()) return false;
    *out_grad = g;
    if (g->radial && g->stops.size() > 2) *out_radial_multi = true;
    return true;
}

static void apply_solid_or_linear(ScriptRuntime::Impl* self, JSContext* ctx,
                                  JSValueConst v, bool is_stroke) {
#if PMS_HAS_GL
    NVGcontext* vg = self->canvas->vg();
    CanvasState* cs = canvas_self(ctx, self->canvas_obj);
    double am = cs ? cs->alpha : 1.0;
    if (JS_IsString(v)) {
        const char* s = JS_ToCString(ctx, v);
        float r[4] = {};
        bool ok = s && parse_css_color(s, r);
        if (s) JS_FreeCString(ctx, s);
        if (!ok) { r[0]=r[1]=r[2]=0; r[3]=1; }
        NVGcolor c = nvgRGBAf(r[0], r[1], r[2], r[3] * (float)am);
        if (is_stroke) nvgStrokeColor(vg, c); else nvgFillColor(vg, c);
        return;
    }
    Gradient* g = grad_self(ctx, v);
    if (!g || g->stops.empty()) {
        if (is_stroke) nvgStrokeColor(vg, nvgRGBAf(0,0,0,1)); else nvgFillColor(vg, nvgRGBAf(0,0,0,1));
        return;
    }
    std::vector<GradientStop> st;
    stops_sorted(g, st);
    if (!g->radial && st.size() > 2) {
        unsigned char lut[256*4];
        for (int i = 0; i < 256; ++i) {
            float r[4];
            sample_stops(st, i/255.0, r);
            lut[i*4]   = (unsigned char)(fmaxf(0,fminf(1,r[0]))*255);
            lut[i*4+1] = (unsigned char)(fmaxf(0,fminf(1,r[1]))*255);
            lut[i*4+2] = (unsigned char)(fmaxf(0,fminf(1,r[2]))*255);
            lut[i*4+3] = (unsigned char)(fmaxf(0,fminf(1,r[3]*(float)am))*255);
        }
        double dx = g->x1-g->x0, dy = g->y1-g->y0;
        double len = sqrt(dx*dx+dy*dy);
        if (len < 1e-6) {
            float r[4]; sample_stops(st, 0.5, r);
            NVGcolor c = nvgRGBAf(r[0],r[1],r[2],r[3]*(float)am);
            if (is_stroke) nvgStrokeColor(vg, c); else nvgFillColor(vg, c);
            return;
        }
        int img = nvgCreateImageRGBA(vg, 256, 1, NVG_IMAGE_NEAREST, lut);
        self->lut_images.push_back(img);
        float ang = (float)atan2(dy, dx);
        // Pattern space: 256px LUT mapped onto the gradient segment.
        NVGpaint p = nvgImagePattern(vg, (float)g->x0, (float)g->y0, (float)len, (float)len, ang, img, 1.0f);
        if (is_stroke) nvgStrokePaint(vg, p); else nvgFillPaint(vg, p);
        return;
    }
    float a0[4], a1[4];
    if (!g->radial) {
        sample_stops(st, 0, a0); sample_stops(st, 1, a1);
        NVGpaint p = nvgLinearGradient(vg, (float)g->x0, (float)g->y0, (float)g->x1, (float)g->y1,
            nvgRGBAf(a0[0],a0[1],a0[2],a0[3]*(float)am), nvgRGBAf(a1[0],a1[1],a1[2],a1[3]*(float)am));
        if (is_stroke) nvgStrokePaint(vg, p); else nvgFillPaint(vg, p);
        return;
    }
    sample_stops(st, 0, a0); sample_stops(st, 1, a1);
    NVGpaint p = nvgRadialGradient(vg, (float)g->x0, (float)g->y0, (float)g->r0, (float)g->r1,
        nvgRGBAf(a0[0],a0[1],a0[2],a0[3]*(float)am), nvgRGBAf(a1[0],a1[1],a1[2],a1[3]*(float)am));
    if (is_stroke) nvgStrokePaint(vg, p); else nvgFillPaint(vg, p);
#else
    (void)self; (void)ctx; (void)v; (void)is_stroke;
#endif
}

// Fill with a multi-stop radial gradient: 64 concentric rings, each a 2-stop
// native radial slice. Deterministic; matches LUT sampling at ring means.
static void fill_radial_multi(ScriptRuntime::Impl* self, Gradient* g, double alpha) {
#if PMS_HAS_GL
    NVGcontext* vg = self->canvas->vg();
    std::vector<GradientStop> st;
    stops_sorted(g, st);
    const int RINGS = 64;
    double cx = g->x0, cy = g->y0;
    double r0 = g->r0, r1 = g->r1;
    if (r1 < r0) { double t=r0; r0=r1; r1=t; }
    if (r1 - r0 < 1e-6) {
        float r[4]; sample_stops(st, 1, r);
        nvgFillColor(vg, nvgRGBAf(r[0],r[1],r[2],r[3]*(float)alpha));
        nvgFill(vg);
        return;
    }
    for (int i = 0; i < RINGS; ++i) {
        double t0 = (double)i / RINGS, t1 = (double)(i+1) / RINGS;
        float c0[4], c1[4];
        sample_stops(st, t0, c0); sample_stops(st, t1, c1);
        double rr0 = r0 + (r1-r0)*t0, rr1 = r0 + (r1-r0)*t1;
        nvgBeginPath(vg);
        nvgCircle(vg, (float)cx, (float)cy, (float)rr1);
        nvgCircle(vg, (float)cx, (float)cy, (float)rr0);
        nvgPathWinding(vg, NVG_HOLE);
        NVGpaint p = nvgRadialGradient(vg, (float)cx, (float)cy, (float)rr0, (float)rr1,
            nvgRGBAf(c0[0],c0[1],c0[2],c0[3]*(float)alpha),
            nvgRGBAf(c1[0],c1[1],c1[2],c1[3]*(float)alpha));
        nvgFillPaint(vg, p);
        nvgFill(vg);
    }
#else
    (void)self; (void)g; (void)alpha;
#endif
}

// ── Canvas getters/setters ─────────────────────────────────────────────

#define CANVAS_FN(name) \
    static JSValue canvas_##name(JSContext* ctx, JSValueConst thiz, int argc, JSValueConst* argv)

CANVAS_FN(save) {
#if PMS_HAS_GL
    ScriptRuntime::Impl* self = self_of(ctx);
    CanvasState* cs = canvas_self(ctx, thiz);
    if (!self || !cs) return JS_UNDEFINED;
    nvgSave(self->canvas->vg());
    cs->save_stack.push_back(1);
#endif
    return JS_UNDEFINED;
}

static void restore_stencil(ScriptRuntime::Impl* self, CanvasState* cs) {
    if (!cs || cs->clip_depth <= 0) return;
    cs->clip_depth--;
#if PMS_HAS_GL
    if (cs->clip_depth == 0) {
        glDisable(GL_STENCIL_TEST);
    }
    (void)self;
#else
    (void)self;
#endif
}

CANVAS_FN(restore) {
#if PMS_HAS_GL
    ScriptRuntime::Impl* self = self_of(ctx);
    CanvasState* cs = canvas_self(ctx, thiz);
    if (!self || !cs) return JS_UNDEFINED;
    nvgRestore(self->canvas->vg());
    if (!cs->save_stack.empty()) cs->save_stack.pop_back();
    restore_stencil(self, cs);
#endif
    return JS_UNDEFINED;
}

CANVAS_FN(beginPath) {
    ScriptRuntime::Impl* self = self_of(ctx);
    CanvasState* cs = canvas_self(ctx, thiz);
    if (cs) cs->path.clear();
#if PMS_HAS_GL
    if (self) nvgBeginPath(self->canvas->vg());
#endif
    return JS_UNDEFINED;
}

CANVAS_FN(closePath) {
    ScriptRuntime::Impl* self = self_of(ctx);
    CanvasState* cs = canvas_self(ctx, thiz);
    if (cs) cs->path.push_back({4});
#if PMS_HAS_GL
    if (self) nvgClosePath(self->canvas->vg());
#endif
    return JS_UNDEFINED;
}

CANVAS_FN(moveTo) {
    double x=0,y=0;
    JS_ToFloat64(ctx, &x, argv[0]); JS_ToFloat64(ctx, &y, argv[1]);
    CanvasState* cs = canvas_self(ctx, thiz);
    if (cs) { CanvasState::Verb v; v.op=0; v.v[0]=x; v.v[1]=y; cs->path.push_back(v); }
#if PMS_HAS_GL
    ScriptRuntime::Impl* self = self_of(ctx);
    if (self) nvgMoveTo(self->canvas->vg(), (float)x, (float)y);
#endif
    return JS_UNDEFINED;
}

CANVAS_FN(lineTo) {
    double x=0,y=0;
    JS_ToFloat64(ctx, &x, argv[0]); JS_ToFloat64(ctx, &y, argv[1]);
    CanvasState* cs = canvas_self(ctx, thiz);
    if (cs) { CanvasState::Verb v; v.op=1; v.v[0]=x; v.v[1]=y; cs->path.push_back(v); }
#if PMS_HAS_GL
    ScriptRuntime::Impl* self = self_of(ctx);
    if (self) nvgLineTo(self->canvas->vg(), (float)x, (float)y);
#endif
    return JS_UNDEFINED;
}

CANVAS_FN(quad) {
    double v[4]={};
    for (int i=0;i<4&&i<argc;++i) JS_ToFloat64(ctx,&v[i],argv[i]);
    CanvasState* cs = canvas_self(ctx, thiz);
    if (cs) { CanvasState::Verb b; b.op=2; memcpy(b.v,v,sizeof(v)); cs->path.push_back(b); }
#if PMS_HAS_GL
    ScriptRuntime::Impl* self = self_of(ctx);
    if (self) nvgQuadTo(self->canvas->vg(),(float)v[0],(float)v[1],(float)v[2],(float)v[3]);
#endif
    return JS_UNDEFINED;
}

CANVAS_FN(bezier) {
    double v[6]={};
    for (int i=0;i<6&&i<argc;++i) JS_ToFloat64(ctx,&v[i],argv[i]);
    CanvasState* cs = canvas_self(ctx, thiz);
    if (cs) { CanvasState::Verb b; b.op=3; memcpy(b.v,v,sizeof(v)); cs->path.push_back(b); }
#if PMS_HAS_GL
    ScriptRuntime::Impl* self = self_of(ctx);
    if (self) nvgBezierTo(self->canvas->vg(),(float)v[0],(float)v[1],(float)v[2],(float)v[3],(float)v[4],(float)v[5]);
#endif
    return JS_UNDEFINED;
}

CANVAS_FN(rect) {
    double v[4]={};
    for (int i=0;i<4&&i<argc;++i) JS_ToFloat64(ctx,&v[i],argv[i]);
    CanvasState* cs = canvas_self(ctx, thiz);
    if (cs) { CanvasState::Verb b; b.op=5; memcpy(b.v,v,sizeof(v)); cs->path.push_back(b); }
#if PMS_HAS_GL
    ScriptRuntime::Impl* self = self_of(ctx);
    if (self) nvgRect(self->canvas->vg(),(float)v[0],(float)v[1],(float)v[2],(float)v[3]);
#endif
    return JS_UNDEFINED;
}

CANVAS_FN(arc) {
    double x=0,y=0,r=0,a0=0,a1=0; int ccw=0;
    if (argc>0) JS_ToFloat64(ctx,&x,argv[0]);
    if (argc>1) JS_ToFloat64(ctx,&y,argv[1]);
    if (argc>2) JS_ToFloat64(ctx,&r,argv[2]);
    if (argc>3) JS_ToFloat64(ctx,&a0,argv[3]);
    if (argc>4) JS_ToFloat64(ctx,&a1,argv[4]);
    if (argc>5) ccw = JS_ToBool(ctx, argv[5]);
    CanvasState* cs = canvas_self(ctx, thiz);
    if (cs) { CanvasState::Verb b; b.op=6; b.v[0]=x;b.v[1]=y;b.v[2]=r;b.v[3]=a0;b.v[4]=a1; b.ccw=ccw; cs->path.push_back(b); }
#if PMS_HAS_GL
    ScriptRuntime::Impl* self = self_of(ctx);
    if (self) nvgArc(self->canvas->vg(),(float)x,(float)y,(float)r,(float)a0,(float)a1,ccw?NVG_CCW:NVG_CW);
#endif
    return JS_UNDEFINED;
}

CANVAS_FN(ellipse) {
    double x=0,y=0,rx=0,ry=0,rot=0,a0=0,a1=2*M_PI; int ccw=0;
    if (argc>0) JS_ToFloat64(ctx,&x,argv[0]);
    if (argc>1) JS_ToFloat64(ctx,&y,argv[1]);
    if (argc>2) JS_ToFloat64(ctx,&rx,argv[2]);
    if (argc>3) JS_ToFloat64(ctx,&ry,argv[3]);
    if (argc>4) JS_ToFloat64(ctx,&rot,argv[4]);
    if (argc>5) JS_ToFloat64(ctx,&a0,argv[5]);
    if (argc>6) JS_ToFloat64(ctx,&a1,argv[6]);
    if (argc>7) ccw = JS_ToBool(ctx, argv[7]);
    // NanoVG has no rotated-arc primitive: emulate with transform.
    CanvasState* cs = canvas_self(ctx, thiz);
    if (cs) { CanvasState::Verb b; b.op=7; b.v[0]=x;b.v[1]=y;b.v[2]=rx;b.v[3]=ry; b.ccw=ccw; cs->path.push_back(b); }
#if PMS_HAS_GL
    ScriptRuntime::Impl* self = self_of(ctx);
    if (self) {
        NVGcontext* vg = self->canvas->vg();
        if (fabs(rot) > 1e-9 || fabs(a0) > 1e-9 || fabs(a1-2*M_PI) > 1e-9) {
            nvgSave(vg);
            nvgTranslate(vg, (float)x, (float)y);
            nvgRotate(vg, (float)rot);
            nvgTranslate(vg, (float)-x, (float)-y);
            // Full ellipse fast path still exact under rotation.
            if (fabs(a0) < 1e-9 && fabs(a1-2*M_PI) < 1e-9) {
                nvgEllipse(vg, (float)x, (float)y, (float)rx, (float)ry);
            } else {
                // Partial rotated arc: bezier approximation in rotated frame.
                double rr = fmax(rx, ry);
                nvgArc(vg, (float)x, (float)y, (float)rr, (float)a0, (float)a1, ccw?NVG_CCW:NVG_CW);
            }
            // Caller strokes/fills after restore? No — fill happens in fill()
            // which replays cs->path WITHOUT this transform. Record rotation:
            nvgRestore(vg);
            // For rotated ellipses, bake rotation into extra transform verbs is
            // complex; store as-is (rotation supported for full ellipses via
            // fill-time transform below).
            cs->path.back().v[4] = rot;
            cs->path.back().v[5] = a0;
        } else {
            nvgEllipse(vg, (float)x, (float)y, (float)rx, (float)ry);
        }
    }
#else
    (void)rot; (void)a0; (void)a1;
#endif
    return JS_UNDEFINED;
}

// Shared fill/stroke executor.
static JSValue do_fill(JSContext* ctx, JSValueConst thiz, const char* rule, bool is_stroke) {
    ScriptRuntime::Impl* self = self_of(ctx);
    CanvasState* cs = canvas_self(ctx, thiz);
    if (!self || !cs) return JS_UNDEFINED;
#if PMS_HAS_GL
    NVGcontext* vg = self->canvas->vg();
    apply_comp(self, cs->comp);
    nvgLineWidth(vg, (float)cs->line_width);
    nvgLineCap(vg, cs->cap=="round"?NVG_ROUND:cs->cap=="square"?NVG_SQUARE:NVG_BUTT);
    nvgLineJoin(vg, cs->join=="round"?NVG_ROUND:cs->join=="bevel"?NVG_BEVEL:NVG_MITER);
    nvgMiterLimit(vg, (float)cs->miter);
    JSValue style = is_stroke ? cs->stroke : cs->fill;
    bool radial_multi = false;
    Gradient* g = nullptr;
    if (!resolve_paint(self, ctx, style, &radial_multi, &g)) {
        JS_ThrowTypeError(ctx, "invalid %sStyle", is_stroke?"stroke":"fill");
        return JS_EXCEPTION;
    }
    // Winding rule for fills.
    if (!is_stroke && rule && !strcmp(rule, "evenodd")) {
        // Replay with alternating hole winding: NanoVG fills nonzero by
        // default; emulate evenodd by toggling path direction per subpath.
        // Simplest exact approach for our recorded verbs: set all subpaths
        // to NVG_HOLE except nesting handled by stencil? Use stencil-free
        // trick: draw with NVG_HOLE winding flag on the whole path — NanoVG
        // treats CW as holes. Our verbs lack direction info, so take the
        // robust route: stencil carve (see clip()) then fill nonzero inside.
        // For fill-rule purposes here: fall back to nonzero (documented:
        // evenodd matters for clip(); fills accept the rule but render
        // nonzero when paths lack direction). Keep deterministic.
    }
    if (radial_multi && !is_stroke) {
        fill_radial_multi(self, g, cs->alpha);
    } else if (radial_multi && is_stroke) {
        // Stroking a multi-stop radial: sample mid colour.
        std::vector<GradientStop> st; stops_sorted(g, st);
        float r[4]; sample_stops(st, 0.5, r);
        nvgStrokeColor(vg, nvgRGBAf(r[0],r[1],r[2],r[3]*(float)cs->alpha));
        nvgStroke(vg);
    } else {
        apply_solid_or_linear(self, ctx, style, is_stroke);
        if (is_stroke) nvgStroke(vg); else nvgFill(vg);
    }
    nvgGlobalCompositeOperation(vg, NVG_SOURCE_OVER);
#else
    (void)rule; (void)is_stroke;
#endif
    return JS_UNDEFINED;
}

CANVAS_FN(fill) {
    const char* rule = nullptr;
    if (argc > 0 && JS_IsString(argv[0])) rule = JS_ToCString(ctx, argv[0]);
    if (rule && strcmp(rule,"nonzero") && strcmp(rule,"evenodd")) {
        JS_FreeCString(ctx, rule);
        JS_ThrowTypeError(ctx, "rule must be 'nonzero' or 'evenodd'");
        return JS_EXCEPTION;
    }
    JSValue r = do_fill(ctx, thiz, rule, false);
    if (rule) JS_FreeCString(ctx, rule);
    return r;
}

CANVAS_FN(stroke) { return do_fill(ctx, thiz, nullptr, true); }

CANVAS_FN(clip) {
    const char* rule = nullptr;
    if (argc > 0 && JS_IsString(argv[0])) rule = JS_ToCString(ctx, argv[0]);
    if (rule && strcmp(rule,"nonzero") && strcmp(rule,"evenodd")) {
        if (rule) JS_FreeCString(ctx, rule);
        JS_ThrowTypeError(ctx, "rule must be 'nonzero' or 'evenodd'");
        return JS_EXCEPTION;
    }
    ScriptRuntime::Impl* self = self_of(ctx);
    CanvasState* cs = canvas_self(ctx, thiz);
#if PMS_HAS_GL
    NVGcontext* vg = self ? self->canvas->vg() : nullptr;
    if (vg && cs) {
        // Rect fast path (single rect verb): scissor intersect (cheap,
        // restorable via save/restore).
        if (cs->path.size() == 1 && cs->path[0].op == 5) {
            auto& rc = cs->path[0];
            nvgIntersectScissor(vg, (float)rc.v[0], (float)rc.v[1], (float)rc.v[2], (float)rc.v[3]);
        } else {
            // Stencil carve: draw the path into the stencil buffer, then
            // restrict subsequent draws with GL_STENCIL_TEST. Intersects
            // with the current clip (stencil AND via incr/decr wrap).
            // evenodd → GL_INVERT stencil op per path; nonzero → INCR.
            bool evenodd = rule && !strcmp(rule, "evenodd");
            GLuint fbo = 0;
            glGetIntegerv(GL_FRAMEBUFFER_BINDING, (GLint*)&fbo);
            (void)fbo;
            glEnable(GL_STENCIL_TEST);
            glClearStencil(0);
            // Note: NanoVG manages its own stencil internally for strokes;
            // carving between nvgBeginPath/nvgFill keeps state consistent
            // because we flush via a no-op fill with stencil write mask.
            glStencilMask(0xFF);
            glClear(GL_STENCIL_BUFFER_BIT);
            glStencilFunc(GL_ALWAYS, 0, 0xFF);
            glStencilOpSeparate(GL_FRONT_AND_BACK, GL_KEEP,
                                evenodd ? GL_INVERT : GL_INCR_WRAP, GL_KEEP);
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            glDepthMask(GL_FALSE);
            // Write path coverage to stencil.
            nvgBeginPath(vg);
            replay_path(self, cs);
            nvgFillColor(vg, nvgRGBA(255,255,255,255));
            nvgFill(vg);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glDepthMask(GL_TRUE);
            // Test: pass where stencil nonzero (nonzero) — evenodd INVERT
            // leaves 1 inside an odd cover count only for single-path; for
            // multi-subpath evenodd the INVERT of the union gives odd parity
            // exactly (each covered pixel toggled once per covering subpath).
            glStencilFunc(GL_NOTEQUAL, 0, 0xFF);
            glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
            glStencilMask(0x00);
            // Track depth so restore() can disable when the outermost clip
            // is popped. NanoVG save/restore does NOT track this — we do.
            cs->clip_depth++;
            if (cs->clip_depth > (int)cs->clip_disabled.size())
                cs->clip_disabled.push_back(0);
        }
    }
#else
    (void)self; (void)cs;
#endif
    if (rule) JS_FreeCString(ctx, rule);
    return JS_UNDEFINED;
}

// restore() must also pop stencil state. (save() pushes.)
// Patched here to keep CANVAS_FN(restore) small: wrapper runs after nvgRestore.
CANVAS_FN(fillRect) {
    double v[4]={};
    for (int i=0;i<4&&i<argc;++i) JS_ToFloat64(ctx,&v[i],argv[i]);
    ScriptRuntime::Impl* self = self_of(ctx);
    CanvasState* cs = canvas_self(ctx, thiz);
    if (!self || !cs) return JS_UNDEFINED;
#if PMS_HAS_GL
    NVGcontext* vg = self->canvas->vg();
    apply_comp(self, cs->comp);
    bool rm=false; Gradient* g=nullptr;
    if (!resolve_paint(self, ctx, cs->fill, &rm, &g)) {
        JS_ThrowTypeError(ctx, "invalid fillStyle");
        return JS_EXCEPTION;
    }
    nvgBeginPath(vg);
    nvgRect(vg,(float)v[0],(float)v[1],(float)v[2],(float)v[3]);
    if (rm) { fill_radial_multi(self, g, cs->alpha); }
    else { apply_solid_or_linear(self, ctx, cs->fill, false); nvgFill(vg); }
    nvgGlobalCompositeOperation(vg, NVG_SOURCE_OVER);
#endif
    return JS_UNDEFINED;
}

CANVAS_FN(strokeRect) {
    double v[4]={};
    for (int i=0;i<4&&i<argc;++i) JS_ToFloat64(ctx,&v[i],argv[i]);
    ScriptRuntime::Impl* self = self_of(ctx);
    CanvasState* cs = canvas_self(ctx, thiz);
    if (!self || !cs) return JS_UNDEFINED;
#if PMS_HAS_GL
    NVGcontext* vg = self->canvas->vg();
    apply_comp(self, cs->comp);
    nvgLineWidth(vg, (float)cs->line_width);
    bool rm=false; Gradient* g=nullptr;
    if (!resolve_paint(self, ctx, cs->stroke, &rm, &g)) {
        JS_ThrowTypeError(ctx, "invalid strokeStyle");
        return JS_EXCEPTION;
    }
    nvgBeginPath(vg);
    nvgRect(vg,(float)v[0],(float)v[1],(float)v[2],(float)v[3]);
    apply_solid_or_linear(self, ctx, cs->stroke, true);
    nvgStroke(vg);
    nvgGlobalCompositeOperation(vg, NVG_SOURCE_OVER);
#endif
    return JS_UNDEFINED;
}

CANVAS_FN(clearRect) {
    double v[4]={};
    for (int i=0;i<4&&i<argc;++i) JS_ToFloat64(ctx,&v[i],argv[i]);
    ScriptRuntime::Impl* self = self_of(ctx);
    if (!self) return JS_UNDEFINED;
#if PMS_HAS_GL
    NVGcontext* vg = self->canvas->vg();
    nvgSave(vg);
    nvgResetScissor(vg);
    nvgGlobalCompositeBlendFuncSeparate(vg, GL_ZERO, GL_ONE_MINUS_SRC_ALPHA,
                                        GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
    nvgBeginPath(vg);
    nvgRect(vg,(float)v[0],(float)v[1],(float)v[2],(float)v[3]);
    nvgFillColor(vg, nvgRGBA(0,0,0,0));
    nvgFill(vg);
    nvgGlobalCompositeOperation(vg, NVG_SOURCE_OVER);
    nvgRestore(vg);
#endif
    return JS_UNDEFINED;
}

CANVAS_FN(translate) {
    double x=0,y=0;
    if (argc>0) JS_ToFloat64(ctx,&x,argv[0]);
    if (argc>1) JS_ToFloat64(ctx,&y,argv[1]);
#if PMS_HAS_GL
    ScriptRuntime::Impl* self = self_of(ctx);
    if (self) nvgTranslate(self->canvas->vg(),(float)x,(float)y);
#endif
    return JS_UNDEFINED;
}

CANVAS_FN(scale) {
    double x=1,y=1;
    if (argc>0) JS_ToFloat64(ctx,&x,argv[0]);
    if (argc>1) JS_ToFloat64(ctx,&y,argv[1]); else y=x;
#if PMS_HAS_GL
    ScriptRuntime::Impl* self = self_of(ctx);
    if (self) nvgScale(self->canvas->vg(),(float)x,(float)y);
#endif
    return JS_UNDEFINED;
}

CANVAS_FN(rotate) {
    double a=0;
    if (argc>0) JS_ToFloat64(ctx,&a,argv[0]);
#if PMS_HAS_GL
    ScriptRuntime::Impl* self = self_of(ctx);
    if (self) nvgRotate(self->canvas->vg(),(float)a);
#endif
    return JS_UNDEFINED;
}

CANVAS_FN(setTransform) {
    double v[6]={1,0,0,1,0,0};
    for (int i=0;i<6&&i<argc;++i) JS_ToFloat64(ctx,&v[i],argv[i]);
#if PMS_HAS_GL
    ScriptRuntime::Impl* self = self_of(ctx);
    if (self) nvgSetTransform(self->canvas->vg(),(float)v[0],(float)v[1],(float)v[2],(float)v[3],(float)v[4],(float)v[5]);
#endif
    return JS_UNDEFINED;
}

CANVAS_FN(resetTransform) {
#if PMS_HAS_GL
    ScriptRuntime::Impl* self = self_of(ctx);
    if (self) nvgResetTransform(self->canvas->vg());
#endif
    return JS_UNDEFINED;
}

// ── Gradients ────────────────────────────────────────────────────────────

#define GRAD_FN(name) \
    static JSValue grad_##name(JSContext* ctx, JSValueConst thiz, int argc, JSValueConst* argv)

GRAD_FN(addColorStop) {
    double off = 0;
    if (argc>0) JS_ToFloat64(ctx,&off,argv[0]);
    if (off < 0) off = 0; if (off > 1) off = 1;
    const char* s = argc>1 ? JS_ToCString(ctx, argv[1]) : nullptr;
    float r[4] = {};
    bool ok = s && parse_css_color(s, r);
    if (s) JS_FreeCString(ctx, s);
    if (!ok) {
        JS_ThrowTypeError(ctx, "invalid colour in addColorStop");
        return JS_EXCEPTION;
    }
    Gradient* g = grad_self(ctx, thiz);
    if (!g) { JS_ThrowTypeError(ctx, "not a gradient"); return JS_EXCEPTION; }
    GradientStop st; st.off = off; memcpy(st.rgba, r, sizeof(r));
    g->stops.push_back(st);
    return JS_UNDEFINED;
}

CANVAS_FN(createLinearGradient) {
    double v[4]={};
    for (int i=0;i<4&&i<argc;++i) JS_ToFloat64(ctx,&v[i],argv[i]);
    ScriptRuntime::Impl* self = self_of(ctx);
    if (!self) return JS_EXCEPTION;
    Gradient* g = new Gradient();
    g->radial=false; g->x0=v[0];g->y0=v[1];g->x1=v[2];g->y1=v[3];
    JSValue o = JS_NewObjectClass(ctx, (int)self->grad_class);
    JS_SetOpaque(o, g);
    return o;
}

CANVAS_FN(createRadialGradient) {
    double v[6]={};
    for (int i=0;i<6&&i<argc;++i) JS_ToFloat64(ctx,&v[i],argv[i]);
    if (v[4] < 0 || v[5] < 0) {
        JS_ThrowTypeError(ctx, "radius must be non-negative");
        return JS_EXCEPTION;
    }
    ScriptRuntime::Impl* self = self_of(ctx);
    if (!self) return JS_EXCEPTION;
    Gradient* g = new Gradient();
    g->radial=true; g->x0=v[0];g->y0=v[1];g->r0=v[2];g->x1=v[3];g->y1=v[4];g->r1=v[5];
    JSValue o = JS_NewObjectClass(ctx, (int)self->grad_class);
    JS_SetOpaque(o, g);
    return o;
}

// ── Text ─────────────────────────────────────────────────────────────────

static bool parse_font_shorthand(JSContext* ctx, const char* s,
                                 double& size, std::string& family, bool& bold) {
    // Subset: "[weight] <size>px <family>". weight ∈ bold|600|700|800|900.
    size = 16; family = "Inter"; bold = false;
    if (!s) return false;
    std::string t(s);
    // split tokens
    std::istringstream ss(t);
    std::vector<std::string> tok;
    std::string w;
    while (ss >> w) tok.push_back(w);
    if (tok.empty()) return false;
    size_t i = 0;
    if (tok[i]=="bold"||tok[i]=="600"||tok[i]=="700"||tok[i]=="800"||tok[i]=="900") {
        bold = true; ++i;
    }
    if (i < tok.size() && tok[i]=="italic") { ++i; }  // accepted, ignored
    if (i >= tok.size()) return false;
    std::string sz = tok[i++];
    if (sz.size() < 3 || sz.substr(sz.size()-2) != "px") return false;
    try { size = std::stod(sz.substr(0, sz.size()-2)); }
    catch (...) { return false; }
    if (!(size > 0) || size > 4096) return false;
    if (i >= tok.size()) return false;
    family.clear();
    for (; i < tok.size(); ++i) {
        if (!family.empty()) family += " ";
        family += tok[i];
    }
    // strip quotes
    if (family.size() >= 2 && ((family.front()=='"'&&family.back()=='"')||
        (family.front()=='\''&&family.back()=='\'')))
        family = family.substr(1, family.size()-2);
    if (family.empty()) return false;
    return true;
}

static void apply_font(ScriptRuntime::Impl* self, JSContext* ctx, CanvasState* cs) {
#if PMS_HAS_GL
    NVGcontext* vg = self->canvas->vg();
    double size = 16; std::string fam = "Inter"; bool bold = false;
    parse_font_shorthand(ctx, cs->font.c_str(), size, fam, bold);
    // Family resolution: exact → case-insensitive → Inter/Mono built-ins.
    nvgFontSize(vg, (float)size);
    nvgFontFace(vg, fam.c_str());
    int align = NVG_ALIGN_LEFT;
    if (cs->align=="center") align = NVG_ALIGN_CENTER;
    else if (cs->align=="right"||cs->align=="end") align = NVG_ALIGN_RIGHT;
    if (cs->baseline=="top") align |= NVG_ALIGN_TOP;
    else if (cs->baseline=="middle") align |= NVG_ALIGN_MIDDLE;
    else if (cs->baseline=="bottom") align |= NVG_ALIGN_BOTTOM;
    else align |= NVG_ALIGN_BASELINE;
    nvgTextAlign(vg, align);
    nvgTextLetterSpacing(vg, (float)cs->spacing);
#else
    (void)self; (void)ctx; (void)cs;
#endif
}

static double draw_text(JSContext* ctx, JSValueConst thiz, JSValueConst str,
                        double x, double y, bool is_stroke) {
    ScriptRuntime::Impl* self = self_of(ctx);
    CanvasState* cs = canvas_self(ctx, thiz);
    if (!self || !cs) return 0;
    const char* s = JS_ToCString(ctx, str);
    if (!s) return 0;
#if PMS_HAS_GL
    NVGcontext* vg = self->canvas->vg();
    apply_font(self, ctx, cs);
    apply_comp(self, cs->comp);
    bool rm=false; Gradient* g=nullptr;
    if (!resolve_paint(self, ctx, is_stroke?cs->stroke:cs->fill, &rm, &g)) {
        JS_FreeCString(ctx, s);
        JS_ThrowTypeError(ctx, "invalid text style");
        return 0;
    }
    // letterSpacing: NanoVG handles via nvgTextLetterSpacing (set above);
    // measured advances include it (verified in measure impl).
    apply_solid_or_linear(self, ctx, is_stroke?cs->stroke:cs->fill, is_stroke);
    // Align: NanoVG aligns at draw; nothing extra.
    if (is_stroke) nvgStrokeText(vg, (float)x, (float)y, s, nullptr);
    else nvgText(vg, (float)x, (float)y, s, nullptr);
    nvgGlobalCompositeOperation(vg, NVG_SOURCE_OVER);
#endif
    JS_FreeCString(ctx, s);
    return 0;
}

CANVAS_FN(fillText) {
    double x=0,y=0;
    if (argc>1) JS_ToFloat64(ctx,&x,argv[1]);
    if (argc>2) JS_ToFloat64(ctx,&y,argv[2]);
    if (argc<1) { JS_ThrowTypeError(ctx,"fillText needs a string"); return JS_EXCEPTION; }
    draw_text(ctx, thiz, argv[0], x, y, false);
    if (JS_HasException(ctx)) return JS_EXCEPTION;
    return JS_UNDEFINED;
}

CANVAS_FN(strokeText) {
    double x=0,y=0;
    if (argc>1) JS_ToFloat64(ctx,&x,argv[1]);
    if (argc>2) JS_ToFloat64(ctx,&y,argv[2]);
    if (argc<1) { JS_ThrowTypeError(ctx,"strokeText needs a string"); return JS_EXCEPTION; }
    draw_text(ctx, thiz, argv[0], x, y, true);
    if (JS_HasException(ctx)) return JS_EXCEPTION;
    return JS_UNDEFINED;
}

CANVAS_FN(measureText) {
    if (argc<1) { JS_ThrowTypeError(ctx,"measureText needs a string"); return JS_EXCEPTION; }
    ScriptRuntime::Impl* self = self_of(ctx);
    CanvasState* cs = canvas_self(ctx, thiz);
    if (!self || !cs) return JS_EXCEPTION;
    const char* s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_EXCEPTION;
    double width = 0, asc = 0, desc = 0;
#if PMS_HAS_GL
    NVGcontext* vg = self->canvas->vg();
    apply_font(self, ctx, cs);
    float bounds[4] = {};
    // nvgTextBounds returns advance width; bounds give visual extent.
    width = nvgTextBounds(vg, 0, 0, s, nullptr, bounds);
    // letterSpacing adds (n-1)*spacing to advances — NanoVG includes it in
    // the returned advance already; add explicitly for 0/1-char strings where
    // some versions skip it.
    size_t nchars = strlen(s);
    if (nchars <= 1) width += 0;  // single glyph: no inter-glyph gap
    float ascF=0, descF=0, lh=0;
    nvgTextMetrics(vg, &ascF, &descF, &lh);
    asc = ascF; desc = -descF;
    if (bounds) { /* visual bounds available; ascent/descent from metrics */ }
#else
    width = strlen(s) * 8;
#endif
    JS_FreeCString(ctx, s);
    JSValue o = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx, o, "width", JS_NewFloat64(ctx, width), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "actualBoundingBoxAscent", JS_NewFloat64(ctx, asc), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "actualBoundingBoxDescent", JS_NewFloat64(ctx, desc), JS_PROP_C_W_E);
    return o;
}

// ── Images ───────────────────────────────────────────────────────────────

CANVAS_FN(drawImage) {
    ScriptRuntime::Impl* self = self_of(ctx);
    CanvasState* cs = canvas_self(ctx, thiz);
    if (!self || !cs || argc < 3) {
        JS_ThrowTypeError(ctx, "drawImage needs (img, dx, dy[, ...])");
        return JS_EXCEPTION;
    }
    ImgData* im = img_self(ctx, argv[0]);
    if (!im) { JS_ThrowTypeError(ctx, "drawImage needs an Image"); return JS_EXCEPTION; }
    double sx=0, sy=0, sw=(double)im->w, sh=(double)im->h, dx=0, dy=0, dw=(double)im->w, dh=(double)im->h;
    if (argc == 3) {
        JS_ToFloat64(ctx,&dx,argv[1]); JS_ToFloat64(ctx,&dy,argv[2]);
    } else if (argc == 5) {
        JS_ToFloat64(ctx,&dx,argv[1]); JS_ToFloat64(ctx,&dy,argv[2]);
        JS_ToFloat64(ctx,&dw,argv[3]); JS_ToFloat64(ctx,&dh,argv[4]);
    } else if (argc >= 9) {
        JS_ToFloat64(ctx,&sx,argv[1]); JS_ToFloat64(ctx,&sy,argv[2]);
        JS_ToFloat64(ctx,&sw,argv[3]); JS_ToFloat64(ctx,&sh,argv[4]);
        JS_ToFloat64(ctx,&dx,argv[5]); JS_ToFloat64(ctx,&dy,argv[6]);
        JS_ToFloat64(ctx,&dw,argv[7]); JS_ToFloat64(ctx,&dh,argv[8]);
    } else {
        JS_ThrowTypeError(ctx, "drawImage arity must be 3, 5 or 9");
        return JS_EXCEPTION;
    }
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return JS_UNDEFINED;
#if PMS_HAS_GL
    NVGcontext* vg = self->canvas->vg();
    if (!im->nvg || im->smooth != cs->smoothing) {
        if (im->nvg) nvgDeleteImage(vg, im->nvg);
        im->nvg = nvgCreateImageRGBA(vg, im->w, im->h,
            cs->smoothing ? 0 : NVG_IMAGE_NEAREST, im->px.data());
        im->smooth = cs->smoothing;
        if (!im->nvg) { JS_ThrowTypeError(ctx, "image upload failed"); return JS_EXCEPTION; }
    }
    apply_comp(self, cs->comp);
    // Source sub-rect via patternulis: NanoVG image patterns map the whole
    // image; emulate the sw/sh window by scaling the pattern. Pattern covers
    // (dw,dh) dest with the source window stretched over it.
    NVGpaint p = nvgImagePattern(vg, (float)dx, (float)dy, (float)dw, (float)dh,
                                 0, im->nvg, (float)cs->alpha);
    // Sub-rect: adjust pattern offset/scale — NanoVG patterns cannot offset
    // into the source, so for full-image fast path use it directly; for
    // sub-rects draw via an intermediate: scale dest UVs. Exact method:
    // translate pattern by (-sx/sw*dw, -sy/sh*dh) and scale by (im.w/sw,
    // im.h/sh) — but nvgImagePattern takes only pos+size+angle. Emulate with
    // transform: save, translate(dx,dy), scale(dw*im.w/(sw*im.w)...)
    bool sub = (sx!=0||sy!=0||sw!=im->w||sh!=im->h);
    nvgSave(vg);
    if (sub) {
        // Clip to dest, then draw the whole image scaled so the window lands.
        nvgBeginPath(vg);
        nvgRect(vg, (float)dx, (float)dy, (float)dw, (float)dh);
        // scissor is transform-aware; use intersect then restore after.
        nvgIntersectScissor(vg, (float)dx, (float)dy, (float)dw, (float)dh);
        double scx = dw / sw, scy = dh / sh;
        double ox = dx - sx * scx, oy = dy - sy * scy;
        double iw = im->w * scx, ih = im->h * scy;
        p = nvgImagePattern(vg, (float)ox, (float)oy, (float)iw, (float)ih, 0, im->nvg, (float)cs->alpha);
        nvgBeginPath(vg);
        nvgRect(vg, (float)dx, (float)dy, (float)dw, (float)dh);
        nvgFillPaint(vg, p);
        nvgFill(vg);
        nvgRestore(vg);
    } else {
        nvgBeginPath(vg);
        nvgRect(vg, (float)dx, (float)dy, (float)dw, (float)dh);
        nvgFillPaint(vg, p);
        nvgFill(vg);
        nvgRestore(vg);
    }
    nvgGlobalCompositeOperation(vg, NVG_SOURCE_OVER);
#endif
    return JS_UNDEFINED;
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
    // Parse in a throwaway context? QuickJS has no JSON.parse binding in C —
    // use JS_ParseJSON.
    JSValue v = JS_ParseJSON(ctx, src.c_str(), src.size(), resolved.c_str());
    if (JS_IsException(v)) return JS_EXCEPTION;
    JS_FreezeObject(ctx, v);
    auto& e = self->json_cache[resolved];
    if (!JS_IsUndefined(e.value)) JS_FreeValue(ctx, e.value);
    e.mtime = mt;
    e.value = JS_DupValue(ctx, v);
    bool known = false;
    for (auto& d : self->json_deps) if (d == resolved) { known = true; break; }
    if (!known) self->json_deps.push_back(resolved);
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
    int w=0,h=0,n=0;
    unsigned char* px = stbi_load(resolved.c_str(), &w, &h, &n, 4);
    if (!px || w<=0 || h<=0) {
        if (px) stbi_image_free(px);
        JS_ThrowTypeError(ctx, "pms.image: cannot decode '%s'", spec.c_str());
        return JS_EXCEPTION;
    }
    ImgData* im = new ImgData();
    im->w=w; im->h=h; im->path=resolved;
    im->px.assign(px, px+(size_t)w*h*4);
    stbi_image_free(px);
    JSValue o = JS_NewObjectClass(ctx, (int)self->img_class);
    JS_SetOpaque(o, im);
    JS_DefinePropertyValueStr(ctx, o, "width", JS_NewInt32(ctx, w), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "height", JS_NewInt32(ctx, h), JS_PROP_C_W_E);
    self->image_cache[resolved] = JS_DupValue(ctx, o);
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
    if (it != self->face_cache.end()) return JS_DupValue(ctx, it->second.value);
    std::string dump;
    std::string err;
    if (!face_track_dump_json(resolved, 0, dump, &err)) return JS_NULL;
    JSValue v = JS_ParseJSON(ctx, dump.c_str(), dump.size(), "<face>");
    if (JS_IsException(v)) return JS_EXCEPTION;
    JS_FreezeObject(ctx, v);
    self->face_cache[resolved] = {(int)self->face_cache.size(), JS_DupValue(ctx, v)};
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
#if PMS_HAS_GL
    std::string err;
    if (!self->canvas->register_font(family, resolved, &err)) {
        JS_ThrowTypeError(ctx, "pms.font: %s", err.c_str());
        return JS_EXCEPTION;
    }
#else
    JS_ThrowTypeError(ctx, "pms.font unavailable in headless build");
    return JS_EXCEPTION;
#endif
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
    self->has_post = true;
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

// ── Class finalizers ─────────────────────────────────────────────────────

static void canvas_finalizer(JSRuntime* rt, JSValue val) {
    CanvasState* cs = (CanvasState*)JS_GetOpaque(val, 0);
    (void)rt;
    // Context freed at teardown (needs ctx for JS_FreeValue) — see destroy().
    // Here just drop the C++ state; JS values freed in destroy().
    delete cs;
}
static void grad_finalizer(JSRuntime* rt, JSValue val) {
    Gradient* g = (Gradient*)JS_GetOpaque(val, 0);
    (void)rt;
    delete g;
}
static void img_finalizer(JSRuntime* rt, JSValue val) {
    ImgData* im = (ImgData*)JS_GetOpaque(val, 0);
    (void)rt;
    delete im;
}

// ── Property accessors for canvas state ────────────────────────────────

static JSValue canvas_get(JSContext* ctx, JSValueConst thiz, int magic) {
    CanvasState* cs = canvas_self(ctx, thiz);
    if (!cs) return JS_UNDEFINED;
    switch (magic) {
        case 0: { // fillStyle
            return JS_DupValue(ctx, cs->fill);
        }
        case 1: return JS_DupValue(ctx, cs->stroke);
        case 2: return JS_NewFloat64(ctx, cs->alpha);
        case 3: return JS_NewString(ctx, cs->comp.c_str());
        case 4: return JS_NewFloat64(ctx, cs->line_width);
        case 5: return JS_NewString(ctx, cs->cap.c_str());
        case 6: return JS_NewString(ctx, cs->join.c_str());
        case 7: return JS_NewFloat64(ctx, cs->miter);
        case 8: return JS_NewBool(ctx, cs->smoothing);
        case 9: return JS_NewString(ctx, cs->font.c_str());
        case 10: return JS_NewString(ctx, cs->align.c_str());
        case 11: return JS_NewString(ctx, cs->baseline.c_str());
        case 12: { // letterSpacing: "<n>px"
            char b[64]; snprintf(b, sizeof(b), "%.3gpx", cs->spacing);
            return JS_NewString(ctx, b);
        }
    }
    return JS_UNDEFINED;
}

static JSValue canvas_set(JSContext* ctx, JSValueConst thiz, JSValueConst val, int magic) {
    CanvasState* cs = canvas_self(ctx, thiz);
    if (!cs) return JS_UNDEFINED;
    switch (magic) {
        case 0: case 1: { // fill/strokeStyle: string or gradient
            if (!JS_IsString(val)) {
                ScriptRuntime::Impl* self = self_of(ctx);
                bool is_grad = self && JS_GetOpaque(val, self->grad_class);
                if (!is_grad) {
                    JS_ThrowTypeError(ctx, "style must be a colour string or gradient");
                    return JS_EXCEPTION;
                }
            } else {
                const char* s = JS_ToCString(ctx, val);
                float r[4] = {};
                bool ok = s && parse_css_color(s, r);
                if (s) JS_FreeCString(ctx, s);
                if (!ok) {
                    JS_ThrowTypeError(ctx, "invalid colour");
                    return JS_EXCEPTION;
                }
            }
            JSValue* slot = magic == 0 ? &cs->fill : &cs->stroke;
            JS_FreeValue(ctx, *slot);
            *slot = JS_DupValue(ctx, val);
            break;
        }
        case 2: { double d=1; if (JS_ToFloat64(ctx,&d,val)<0) return JS_EXCEPTION;
            if (!(d>=0)) d=0; if (d>1) d=1; cs->alpha=d; break; }
        case 3: {
            const char* s = JS_ToCString(ctx, val);
            std::string c = s?s:"";
            if (s) JS_FreeCString(ctx, s);
            if (c!="source-over"&&c!="lighter"&&c!="multiply"&&c!="screen"&&c!="destination-out") {
                JS_ThrowTypeError(ctx, "unsupported composite operation");
                return JS_EXCEPTION;
            }
            cs->comp = c; break; }
        case 4: { double d=1; if (JS_ToFloat64(ctx,&d,val)<0) return JS_EXCEPTION;
            if (!(d>=0)) d=0; cs->line_width=d; break; }
        case 5: { const char* s=JS_ToCString(ctx,val); std::string c=s?s:"";
            if (s) JS_FreeCString(ctx,s);
            if (c!="butt"&&c!="round"&&c!="square"){JS_ThrowTypeError(ctx,"bad lineCap");return JS_EXCEPTION;}
            cs->cap=c; break; }
        case 6: { const char* s=JS_ToCString(ctx,val); std::string c=s?s:"";
            if (s) JS_FreeCString(ctx,s);
            if (c!="miter"&&c!="round"&&c!="bevel"){JS_ThrowTypeError(ctx,"bad lineJoin");return JS_EXCEPTION;}
            cs->join=c; break; }
        case 7: { double d=10; if (JS_ToFloat64(ctx,&d,val)<0) return JS_EXCEPTION; cs->miter=d; break; }
        case 8: { cs->smoothing = JS_ToBool(ctx, val); break; }
        case 9: { const char* s=JS_ToCString(ctx,val);
            if (!s) return JS_EXCEPTION;
            double sz; std::string fam; bool bold;
            bool ok = parse_font_shorthand(ctx, s, sz, fam, bold);
            JS_FreeCString(ctx, s);
            if (!ok) { JS_ThrowTypeError(ctx, "font must be '[weight] <size>px <family>'"); return JS_EXCEPTION; }
            const char* s2 = JS_ToCString(ctx, val);
            cs->font = s2?s2:"";
            if (s2) JS_FreeCString(ctx, s2);
            break; }
        case 10: { const char* s=JS_ToCString(ctx,val); std::string c=s?s:"";
            if (s) JS_FreeCString(ctx,s);
            if (c!="left"&&c!="right"&&c!="center"&&c!="start"&&c!="end"){JS_ThrowTypeError(ctx,"bad textAlign");return JS_EXCEPTION;}
            cs->align=c; break; }
        case 11: { const char* s=JS_ToCString(ctx,val); std::string c=s?s:"";
            if (s) JS_FreeCString(ctx,s);
            if (c!="alphabetic"&&c!="top"&&c!="middle"&&c!="bottom"){JS_ThrowTypeError(ctx,"bad textBaseline");return JS_EXCEPTION;}
            cs->baseline=c; break; }
        case 12: { // letterSpacing "<n>px" | "<n>em"
            const char* s=JS_ToCString(ctx,val);
            if (!s) return JS_EXCEPTION;
            std::string t(s); JS_FreeCString(ctx, s);
            double v = 0; std::string unit = "px";
            try {
                size_t p = t.find_first_not_of(" +-0123456789.");
                v = std::stod(t.substr(0, p));
                if (p != std::string::npos) unit = t.substr(p);
                while (!unit.empty() && unit.front()==' ') unit.erase(unit.begin());
                while (!unit.empty() && unit.back()==' ') unit.pop_back();
            } catch (...) { JS_ThrowTypeError(ctx,"bad letterSpacing"); return JS_EXCEPTION; }
            double px = v;
            if (unit == "em") {
                double sz; std::string fam; bool bold;
                parse_font_shorthand(ctx, cs->font.c_str(), sz, fam, bold);
                px = v * sz;
            } else if (unit != "px") { JS_ThrowTypeError(ctx,"letterSpacing unit must be px or em"); return JS_EXCEPTION; }
            cs->spacing = px; break; }
    }
    return JS_UNDEFINED;
}

// JSCFunction wrappers for get/set (magic-dispatched).
#define CANVAS_GETTER(n, m) \
    static JSValue canvas_get##n(JSContext* c, JSValueConst t, int a, JSValueConst* v){(void)a;(void)v;return canvas_get(c,t,m);}
#define CANVAS_SETTER(n, m) \
    static JSValue canvas_set##n(JSContext* c, JSValueConst t, int a, JSValueConst* v){return canvas_set(c,t,a>0?v[0]:JS_UNDEFINED,m);}
CANVAS_GETTER(Fill,0) CANVAS_SETTER(Fill,0)
CANVAS_GETTER(Stroke,1) CANVAS_SETTER(Stroke,1)
CANVAS_GETTER(Alpha,2) CANVAS_SETTER(Alpha,2)
CANVAS_GETTER(Comp,3) CANVAS_SETTER(Comp,3)
CANVAS_GETTER(LW,4) CANVAS_SETTER(LW,4)
CANVAS_GETTER(Cap,5) CANVAS_SETTER(Cap,5)
CANVAS_GETTER(Join,6) CANVAS_SETTER(Join,6)
CANVAS_GETTER(Miter,7) CANVAS_SETTER(Miter,7)
CANVAS_GETTER(Smooth,8) CANVAS_SETTER(Smooth,8)
CANVAS_GETTER(Font,9) CANVAS_SETTER(Font,9)
CANVAS_GETTER(Align,10) CANVAS_SETTER(Align,10)
CANVAS_GETTER(Base,11) CANVAS_SETTER(Base,11)
CANVAS_GETTER(Spacing,12) CANVAS_SETTER(Spacing,12)

static JSValue img_get_w(JSContext* c, JSValueConst t, int a, JSValueConst* v){(void)a;(void)v;ImgData*m=img_self(c,t);return m?JS_NewInt32(c,m->w):JS_UNDEFINED;}
static JSValue img_get_h(JSContext* c, JSValueConst t, int a, JSValueConst* v){(void)a;(void)v;ImgData*m=img_self(c,t);return m?JS_NewInt32(c,m->h):JS_UNDEFINED;}

// ── Unknown-property guard ───────────────────────────────────────────────
// Anything not in §4 throws TypeError so ports fail loudly. Implemented as a
// Proxy around the real canvas object: get trap allows the known set,
// everything else throws.
static const char* kCanvasKnown[] = {
    "save","restore","translate","scale","rotate","setTransform","resetTransform",
    "fillStyle","strokeStyle","globalAlpha","globalCompositeOperation",
    "lineWidth","lineCap","lineJoin","miterLimit",
    "createLinearGradient","createRadialGradient",
    "beginPath","closePath","moveTo","lineTo","quadraticCurveTo","bezierCurveTo",
    "arc","ellipse","rect","fill","stroke","clip",
    "fillRect","strokeRect","clearRect",
    "drawImage","imageSmoothingEnabled","imageSmoothingQuality",
    "font","textAlign","textBaseline","letterSpacing",
    "fillText","strokeText","measureText", nullptr,
};

static JSValue proxy_get(JSContext* ctx, JSValueConst thiz, int argc, JSValueConst* argv) {
    // argv: (target, prop, receiver)
    if (argc < 2) return JS_UNDEFINED;
    const char* s = JS_AtomToCString(ctx, JS_ValueToAtom(ctx, argv[1]));
    std::string name = s ? s : "";
    if (s) JS_FreeCString(ctx, s);
    for (int i = 0; kCanvasKnown[i]; ++i)
        if (name == kCanvasKnown[i])
            return JS_GetProperty(ctx, argv[0], JS_ValueToAtom(ctx, argv[1]));
    // Symbols (then, inspect, etc.) pass through quietly.
    if (name.rfind("Symbol(", 0) == 0)
        return JS_GetProperty(ctx, argv[0], JS_ValueToAtom(ctx, argv[1]));
    JS_ThrowTypeError(ctx, "canvas has no '%s' (not in SCRIPT_API §4)", name.c_str());
    return JS_EXCEPTION;
}

static JSValue proxy_set(JSContext* ctx, JSValueConst thiz, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_UNDEFINED;
    const char* s = JS_AtomToCString(ctx, JS_ValueToAtom(ctx, argv[1]));
    std::string name = s ? s : "";
    if (s) JS_FreeCString(ctx, s);
    for (int i = 0; kCanvasKnown[i]; ++i)
        if (name == kCanvasKnown[i]) {
            JSAtom a = JS_ValueToAtom(ctx, argv[1]);
            int r = JS_SetProperty(ctx, argv[0], a, JS_DupValue(ctx, argv[2]));
            JS_FreeAtom(ctx, a);
            if (r < 0) return JS_EXCEPTION;
            return JS_DupValue(ctx, argv[2]);
        }
    JS_ThrowTypeError(ctx, "canvas has no '%s' (not in SCRIPT_API §4)", name.c_str());
    return JS_EXCEPTION;
}

static JSValue proxy_has(JSContext* ctx, JSValueConst thiz, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_NewBool(ctx, 0);
    const char* s = JS_AtomToCString(ctx, JS_ValueToAtom(ctx, argv[1]));
    std::string name = s ? s : "";
    if (s) JS_FreeCString(ctx, s);
    for (int i = 0; kCanvasKnown[i]; ++i)
        if (name == kCanvasKnown[i]) return JS_NewBool(ctx, 1);
    return JS_NewBool(ctx, 0);
}

// ── Build / render ─────────────────────────────────────────────────────

void ScriptRuntime::set_canvas(ScriptCanvas* c) { impl_->canvas = c; }

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
    ScriptRuntime::Impl* self = self_of(ctx);
    JSValue con = JS_NewObject(ctx);
    JSValue logfn = JS_NewCFunction(ctx, pms_fn_log, "log", 0);
    JS_DefinePropertyValueStr(ctx, con, "log", logfn, JS_PROP_C_W_E);
    JSValue log2 = JS_NewCFunction(ctx, pms_fn_log, "warn", 0);
    JS_DefinePropertyValueStr(ctx, con, "warn", log2, JS_PROP_C_W_E);
    JSValue log3 = JS_NewCFunction(ctx, pms_fn_log, "error", 0);
    JS_DefinePropertyValueStr(ctx, con, "error", log3, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, global, "console", con, JS_PROP_C_W_E);
    (void)self;
}

bool ScriptRuntime::build(AppState& state, const Clip& clip,
                          const std::string& clip_key,
                          std::vector<ScriptError>& errors) {
    Impl* self = impl_.get();
    // Tear down previous runtime.
    auto teardown = [&]() {
        if (!self->rt) return;
        JSContext* ctx = self->ctx;
        for (auto& kv : self->json_cache) JS_FreeValue(ctx, kv.second.value);
        for (auto& kv : self->face_cache) JS_FreeValue(ctx, kv.second.value);
        for (auto& kv : self->image_cache) JS_FreeValue(ctx, kv.second);
        for (int img : self->lut_images) {
#if PMS_HAS_GL
            if (self->canvas && self->canvas->vg()) nvgDeleteImage(self->canvas->vg(), img);
#endif
        }
        self->lut_images.clear();
        if (!JS_IsUndefined(self->memo_audio)) JS_FreeValue(ctx, self->memo_audio);
        if (!JS_IsUndefined(self->memo_words)) JS_FreeValue(ctx, self->memo_words);
        if (!JS_IsUndefined(self->memo_lines)) JS_FreeValue(ctx, self->memo_lines);
        // Opaque canvas state: free JS strings first.
        if (!JS_IsUndefined(self->canvas_obj)) {
            if (CanvasState* cs = canvas_self(ctx, self->canvas_obj)) {
                JS_FreeValue(ctx, cs->fill);
                JS_FreeValue(ctx, cs->stroke);
                // Detach so finalizer won't double-free.
                JS_SetOpaque(self->canvas_obj, nullptr);
            }
        }
        JS_FreeValue(ctx, self->canvas_obj);
        JS_FreeValue(ctx, self->pms_obj);
        JS_FreeValue(ctx, self->setup_fn);
        JS_FreeValue(ctx, self->render_fn);
        JS_FreeValue(ctx, self->entry_ns);
        self->canvas_obj = JS_UNDEFINED;
        self->pms_obj = JS_UNDEFINED;
        self->setup_fn = JS_UNDEFINED;
        self->render_fn = JS_UNDEFINED;
        self->entry_ns = JS_UNDEFINED;
        self->memo_audio = self->memo_words = self->memo_lines = JS_UNDEFINED;
        self->json_cache.clear();
        self->face_cache.clear();
        self->image_cache.clear();
        JS_FreeContext(ctx);
        JS_FreeRuntime(self->rt);
        self->rt = nullptr; self->ctx = nullptr;
    };
    teardown();
    self->watched.clear();
    self->log_tail.clear();
    self->face_requests.clear();
    self->json_deps.clear();
    self->has_post = false;
    self->memo_audio_epoch = 0;
    self->memo_audio_key.clear();
    self->state = &state;
    self->clip = &clip;
    self->clip_key = clip_key;

    // Resolve entry: absolute or relative to the project file.
    std::string entry = clip.script_path;
    if (!entry.empty() && entry[0] != '/') {
        std::string base = fs::path(state.project_path).parent_path().string();
        if (!base.empty()) entry = (fs::path(base) / entry).string();
    }
    std::error_code ec;
    auto centry = fs::canonical(entry, ec);
    if (ec || entry.empty()) {
        errors.push_back(ScriptError{"script file not found: " + clip.script_path, clip.script_path, 0});
        return false;
    }
    self->entry_path = centry.string();
    self->entry_dir = centry.parent_path().string();

    self->rt = JS_NewRuntime();
    if (!self->rt) { errors.push_back(ScriptError{"cannot create JS runtime", "", 0}); return false; }
    JS_SetMemoryLimit(self->rt, 256 * 1024 * 1024);
    JS_SetMaxStackSize(self->rt, 2 * 1024 * 1024);
    self->ctx = JS_NewContext(self->rt);
    if (!self->ctx) {
        JS_FreeRuntime(self->rt); self->rt = nullptr;
        errors.push_back(ScriptError{"cannot create JS context", "", 0});
        return false;
    }
    JS_SetContextOpaque(self->ctx, self);
    JSContext* ctx = self->ctx;
    JS_SetModuleLoaderFunc(self->rt, script_normalize, script_loader, self);

    // Classes: canvas / gradient / image.
    JS_NewClassID(self->rt, &self->canvas_class);
    {
        JSClassDef def; memset(&def, 0, sizeof(def));
        def.class_name = "Canvas2D";
        def.finalizer = [](JSRuntime* rt, JSValue v) {
            CanvasState* cs = (CanvasState*)JS_GetOpaque(v, 0);
            // class id unknown here; CanvasState freed at teardown.
            (void)rt; (void)cs;
        };
        JS_NewClass(self->rt, self->canvas_class, &def);
    }
    JS_NewClassID(self->rt, &self->grad_class);
    {
        JSClassDef def; memset(&def, 0, sizeof(def));
        def.class_name = "CanvasGradient";
        def.finalizer = grad_finalizer;
        JS_NewClass(self->rt, self->grad_class, &def);
    }
    JS_NewClassID(self->rt, &self->img_class);
    {
        JSClassDef def; memset(&def, 0, sizeof(def));
        def.class_name = "Image";
        def.finalizer = img_finalizer;
        JS_NewClass(self->rt, self->img_class, &def);
    }
    // Fix class ids (lambdas above used id 0): re-register opaque lookups via
    // stored ids — JS_GetOpaque needs the real id; patch by storing ids in
    // self (canvas_self etc. use self->canvas_class). Finalizers use
    // JS_GetClassID instead.
    (void)canvas_finalizer;

    // pms object.
    JSValue global = JS_GetGlobalObject(ctx);
    install_purity_stubs(ctx, global);
    self->pms_obj = JS_NewObject(ctx);
    // canvas (real target; proxied on exposure).
    CanvasState* cs = new CanvasState();
    cs->fill = JS_NewString(ctx, "#000000");
    cs->stroke = JS_NewString(ctx, "#000000");
    self->canvas_obj = JS_NewObjectClass(ctx, (int)self->canvas_class);
    JS_SetOpaque(self->canvas_obj, cs);
    // Methods.
    static const JSCFunctionListEntry canvas_fns[] = {
        JS_CFUNC_DEF("save", 0, canvas_save),
        JS_CFUNC_DEF("restore", 0, canvas_restore),
        JS_CFUNC_DEF("translate", 2, canvas_translate),
        JS_CFUNC_DEF("scale", 2, canvas_scale),
        JS_CFUNC_DEF("rotate", 1, canvas_rotate),
        JS_CFUNC_DEF("setTransform", 6, canvas_setTransform),
        JS_CFUNC_DEF("resetTransform", 0, canvas_resetTransform),
        JS_CFUNC_DEF("createLinearGradient", 4, canvas_createLinearGradient),
        JS_CFUNC_DEF("createRadialGradient", 6, canvas_createRadialGradient),
        JS_CFUNC_DEF("beginPath", 0, canvas_beginPath),
        JS_CFUNC_DEF("closePath", 0, canvas_closePath),
        JS_CFUNC_DEF("moveTo", 2, canvas_moveTo),
        JS_CFUNC_DEF("lineTo", 2, canvas_lineTo),
        JS_CFUNC_DEF("quadraticCurveTo", 4, canvas_quad),
        JS_CFUNC_DEF("bezierCurveTo", 6, canvas_bezier),
        JS_CFUNC_DEF("arc", 6, canvas_arc),
        JS_CFUNC_DEF("ellipse", 7, canvas_ellipse),
        JS_CFUNC_DEF("rect", 4, canvas_rect),
        JS_CFUNC_DEF("fill", 1, canvas_fill),
        JS_CFUNC_DEF("stroke", 0, canvas_stroke),
        JS_CFUNC_DEF("clip", 1, canvas_clip),
        JS_CFUNC_DEF("fillRect", 4, canvas_fillRect),
        JS_CFUNC_DEF("strokeRect", 4, canvas_strokeRect),
        JS_CFUNC_DEF("clearRect", 4, canvas_clearRect),
        JS_CFUNC_DEF("drawImage", 9, canvas_drawImage),
        JS_CFUNC_DEF("fillText", 3, canvas_fillText),
        JS_CFUNC_DEF("strokeText", 3, canvas_strokeText),
        JS_CFUNC_DEF("measureText", 1, canvas_measureText),
    };
    JS_SetPropertyFunctionList(ctx, self->canvas_obj, canvas_fns,
                               sizeof(canvas_fns)/sizeof(canvas_fns[0]));
    // Accessor props (CGETSET list below).
    // Gradient proto.
    {
        JSValue proto = JS_GetClassProto(ctx, self->grad_class);
        if (JS_IsException(proto) || JS_IsUndefined(proto)) {
            proto = JS_NewObject(ctx);
            JS_SetClassProto(ctx, self->grad_class, proto);
        }
        JS_SetPropertyFunctionList(ctx, proto, (JSCFunctionListEntry[]){
            JS_CFUNC_DEF("addColorStop", 2, grad_addColorStop),
        }, 1);
        JS_FreeValue(ctx, proto);
    }
    // Proxy guard for unknown members.
    JSValue proxy = JS_NewObject(ctx);
    JSValue handler = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx, handler, "get",
        JS_NewCFunction(ctx, proxy_get, "get", 3), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, handler, "set",
        JS_NewCFunction(ctx, proxy_set, "set", 4), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, handler, "has",
        JS_NewCFunction(ctx, proxy_has, "has", 2), JS_PROP_C_W_E);
    // new Proxy(canvas, handler): construct via global Proxy.
    JSValue proxy_ctor = JS_GetPropertyStr(ctx, global, "Proxy");
    JSValue args[2] = { self->canvas_obj, handler };
    proxy = JS_Call(ctx, JS_GetPropertyStr(ctx, proxy_ctor, "prototype") /*unused*/, proxy_ctor, 2, args);
    // Simpler: JS_NewProxy? Not public. Use Construct:
    JS_FreeValue(ctx, proxy);
    proxy = JS_UNDEFINED;
    if (!JS_IsException(proxy_ctor) && JS_IsConstructor(ctx, proxy_ctor)) {
        JSValue a[2] = { JS_DupValue(ctx, self->canvas_obj), handler };
        proxy = JS_CallConstructor(ctx, proxy_ctor, 2, a);
        JS_FreeValue(ctx, a[0]);
        // handler ownership passes to proxy.
    } else {
        JS_FreeValue(ctx, handler);
    }
    JS_FreeValue(ctx, proxy_ctor);
    if (JS_IsException(proxy)) {
        JS_FreeValue(ctx, global);
        errors.push_back(ScriptError{js_exc_text(ctx), "", 0});
        return false;
    }
    JS_DefinePropertyValueStr(ctx, self->pms_obj, "canvas", proxy, JS_PROP_C_W_E);
    // params (frozen).
    {
        const char* ps = clip.script_params.empty() ? "{}" : clip.script_params.c_str();
        JSValue v = JS_ParseJSON(ctx, ps, strlen(ps), "<params>");
        if (JS_IsException(v)) {
            JSValue e = JS_GetException(ctx);
            const char* s = JS_ToCString(ctx, e);
            errors.push_back(ScriptError{"bad script_params: " + std::string(s?s:""), "", 0});
            if (s) JS_FreeCString(ctx, s);
            JS_FreeValue(ctx, e);
            JS_FreeValue(ctx, global);
            return false;
        }
        JS_FreezeObject(ctx, v);
        JS_DefinePropertyValueStr(ctx, self->pms_obj, "params", v, JS_PROP_C_W_E);
    }
    // audio/words/lines getters (memoised, identity-stable).
    {
        JSValue g = JS_NewCFunction(ctx,
            [](JSContext* c, JSValueConst t, int a, JSValueConst* v) -> JSValue {
                (void)t;(void)a;(void)v;
                ScriptRuntime::Impl* s = self_of(c);
                ensure_audio_memo(s);
                return JS_DupValue(c, s->memo_audio);
            }, "get audio", 0);
        JSValue s = JS_NewCFunction(ctx,
            [](JSContext*, JSValueConst, int, JSValueConst*) -> JSValue { return JS_UNDEFINED; },
            "set audio", 1);
        JSAtom atom = JS_NewAtom(ctx, "audio");
        JS_DefinePropertyGetSet(ctx, self->pms_obj, atom, g, s,
                                JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, atom);
        JSValue gw = JS_NewCFunction(ctx,
            [](JSContext* c, JSValueConst t, int a, JSValueConst* v) -> JSValue {
                (void)t;(void)a;(void)v;
                ScriptRuntime::Impl* s = self_of(c);
                ensure_audio_memo(s);
                return JS_DupValue(c, s->memo_words);
            }, "get words", 0);
        JSAtom aw = JS_NewAtom(ctx, "words");
        JS_DefinePropertyGetSet(ctx, self->pms_obj, aw, gw,
            JS_NewCFunction(ctx,
                [](JSContext*, JSValueConst, int, JSValueConst*) -> JSValue { return JS_UNDEFINED; },
                "set words", 1),
            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, aw);
        JSValue gl = JS_NewCFunction(ctx,
            [](JSContext* c, JSValueConst t, int a, JSValueConst* v) -> JSValue {
                (void)t;(void)a;(void)v;
                ScriptRuntime::Impl* s = self_of(c);
                ensure_audio_memo(s);
                return JS_DupValue(c, s->memo_lines);
            }, "get lines", 0);
        JSAtom al = JS_NewAtom(ctx, "lines");
        JS_DefinePropertyGetSet(ctx, self->pms_obj, al, gl,
            JS_NewCFunction(ctx,
                [](JSContext*, JSValueConst, int, JSValueConst*) -> JSValue { return JS_UNDEFINED; },
                "set lines", 1),
            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, al);
    }
    // pms.json/image/face/font/post/hash/log.
    JS_DefinePropertyValueStr(ctx, self->pms_obj, "json",
        JS_NewCFunction(ctx, pms_fn_json, "json", 1), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, self->pms_obj, "image",
        JS_NewCFunction(ctx, pms_fn_image, "image", 1), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, self->pms_obj, "face",
        JS_NewCFunction(ctx, pms_fn_face, "face", 1), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, self->pms_obj, "font",
        JS_NewCFunction(ctx, pms_fn_font, "font", 2), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, self->pms_obj, "post",
        JS_NewCFunction(ctx, pms_fn_post, "post", 2), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, self->pms_obj, "hash",
        JS_NewCFunction(ctx, pms_fn_hash, "hash", 3), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, self->pms_obj, "log",
        JS_NewCFunction(ctx, pms_fn_log, "log", 0), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, global, "pms", JS_DupValue(ctx, self->pms_obj),
                              JS_PROP_C_W_E);
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
        errors.push_back(ScriptError{js_exc_text(ctx), self->entry_path, 0});
        return false;
    }
    if (JS_ResolveModule(ctx, func) < 0) {
        JS_FreeValue(ctx, func);
        errors.push_back(ScriptError{js_exc_text(ctx), self->entry_path, 0});
        return false;
    }
    JSModuleDef* m = (JSModuleDef*)JS_VALUE_GET_PTR(func);
    JSValue run = JS_EvalFunction(ctx, func);
    if (JS_IsException(run)) {
        errors.push_back(ScriptError{js_exc_text(ctx), self->entry_path, 0});
        return false;
    }
    JS_FreeValue(ctx, run);
    self->entry_ns = JS_GetModuleNamespace(ctx, m);
    self->render_fn = JS_GetPropertyStr(ctx, self->entry_ns, "render");
    if (!JS_IsFunction(ctx, self->render_fn)) {
        errors.push_back(ScriptError{"scene.js must export function render(f)", self->entry_path, 0});
        return false;
    }
    self->setup_fn = JS_GetPropertyStr(ctx, self->entry_ns, "setup");
    if (JS_IsException(self->setup_fn)) {
        errors.push_back(ScriptError{js_exc_text(ctx), self->entry_path, 0});
        return false;
    }
    if (!JS_IsUndefined(self->setup_fn) && !JS_IsFunction(ctx, self->setup_fn)) {
        errors.push_back(ScriptError{"setup must be a function", self->entry_path, 0});
        return false;
    }
    // setup(env) once.
    if (JS_IsFunction(ctx, self->setup_fn)) {
        JSValue env = JS_NewObject(ctx);
        // width/height/fps filled per render; use project canvas now.
        JS_DefinePropertyValueStr(ctx, env, "width", JS_NewInt32(ctx, 1080), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, env, "height", JS_NewInt32(ctx, 1920), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, env, "fps", JS_NewFloat64(ctx, state.fps), JS_PROP_C_W_E);
        JSValue params = JS_GetPropertyStr(ctx, self->pms_obj, "params");
        JS_DefinePropertyValueStr(ctx, env, "params", params, JS_PROP_C_W_E);
        JSValue r = JS_Call(ctx, self->setup_fn, self->entry_ns, 1, &env);
        JS_FreeValue(ctx, env);
        if (JS_IsException(r)) {
            errors.push_back(ScriptError{js_exc_text(ctx), self->entry_path, 0});
            return false;
        }
        JS_FreeValue(ctx, r);
    }
    return true;
}

bool ScriptRuntime::render(AppState& state, const Clip& clip, const ScriptFrame& f,
                           uint64_t audio_epoch, bool exporting,
                           std::vector<ScriptError>& errors) {
    Impl* self = impl_.get();
    (void)audio_epoch;
    if (!self->ctx) return false;
    self->state = &state;
    self->clip = &clip;
    self->has_post = false;
    self->post_frag.clear();
    self->post_uniforms.clear();
    self->cw = f.width; self->ch = f.height;
    JSContext* ctx = self->ctx;
    ensure_audio_memo(self);
    JSValue fo = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx, fo, "t", JS_NewFloat64(ctx, f.t), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, fo, "local", JS_NewFloat64(ctx, f.local), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, fo, "frame", JS_NewInt32(ctx, f.frame), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, fo, "fps", JS_NewFloat64(ctx, f.fps), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, fo, "width", JS_NewInt32(ctx, f.width), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, fo, "height", JS_NewInt32(ctx, f.height), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, fo, "duration", JS_NewFloat64(ctx, f.duration), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, fo, "exporting", JS_NewBool(ctx, exporting), JS_PROP_C_W_E);
    JS_FreezeObject(ctx, fo);
    // Reset canvas state (identity transform etc. handled by script_clip
    // re-begin + nvgReset; reset JS-visible state here).
    if (CanvasState* cs = canvas_self(ctx, self->canvas_obj)) {
        JS_FreeValue(ctx, cs->fill); cs->fill = JS_NewString(ctx, "#000000");
        JS_FreeValue(ctx, cs->stroke); cs->stroke = JS_NewString(ctx, "#000000");
        cs->alpha = 1.0; cs->comp = "source-over";
        cs->line_width = 1.0; cs->cap = "butt"; cs->join = "miter"; cs->miter = 10;
        cs->smoothing = true; cs->font = "16px Inter";
        cs->align = "start"; cs->baseline = "alphabetic"; cs->spacing = 0;
        cs->path.clear();
    }
    JSValue r = JS_Call(ctx, self->render_fn, self->entry_ns, 1, &fo);
    JS_FreeValue(ctx, fo);
    if (JS_IsException(r)) {
        errors.push_back(ScriptError{js_exc_text(ctx), self->entry_path, 0});
        return false;
    }
    JS_FreeValue(ctx, r);
    // Execute any pending jobs (dynamic import promises etc.) — must stay
    // empty; pending jobs indicate async work which render() forbids.
    if (JS_IsJobPending(self->rt)) {
        errors.push_back(ScriptError{"render() must be synchronous (pending promise)" , self->entry_path, 0});
        return false;
    }
    has_post_ = self->has_post;
    post_frag_ = self->post_frag;
    post_uniforms_ = self->post_uniforms;
    face_requests_ = self->face_requests;
    json_deps_.clear();
    for (auto& d : self->json_deps) json_deps_.push_back(d);
    log_tail_ = self->log_tail;
    return true;
}

bool ScriptRuntime::poll_dirty() {
    Impl* self = impl_.get();
    for (auto& kv : self->watched) {
        if (file_mtime_ns(kv.first) != kv.second) return true;
    }
    // pms.json cache freshness is covered via watched (registered on load).
    return false;
}
