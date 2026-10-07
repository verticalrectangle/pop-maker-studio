// Per-clip Script compositing — see script_clip.h.
#include "platform.h"
#include "script_clip.h"
#include "app.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#if PMS_HAS_GL
#include "face_cache.h"
#include "script_context2d.h"
#include "script_gpu.h"
#include "script_runtime.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkPaint.h"
#include "include/core/SkTypeface.h"

#include <chrono>
#endif

std::string script_clip_key(int track, int clip) {
    return std::to_string(track) + ":" + std::to_string(clip);
}

#if PMS_HAS_GL

namespace {

constexpr size_t kMaxErrors = 8;

struct Entry {
    ScriptRuntime rt;
    ScriptSurface surface;
    std::string script_path, script_params;
    bool built = false;        // build attempted for script_path/params
    bool build_ok = false;
    bool force_build = false;
    std::vector<ScriptError> build_errors;
    // §9 cache key of the texture's current contents.
    long last_frame = -1;
    int last_w = -1, last_h = -1, last_sw = -1, last_sh = -1;
    uint64_t last_audio = 0;
    bool last_exporting = false;
    bool faces_waiting = false;  // a pms.face() returned null: re-render when ready
    std::vector<ScriptError> errors;
    double render_ms = 0.0, flush_ms = 0.0;
    int builds = 0;
};

std::map<std::string, std::unique_ptr<Entry>> g_entries;

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// Red card listing the errors (preview feedback; export fails instead).
void draw_error_card(SkCanvas* c, int w, int h, const std::vector<ScriptError>& errors) {
    SkPaint bg;
    bg.setColor4f({0.45f, 0.02f, 0.02f, 0.88f});
    c->drawRect(SkRect::MakeWH((float)w, (float)h), bg);
    float size = std::max(14.f, (float)std::min(w, h) / 42.f);
    SkFont font(c2d_builtin_typeface("Mono", 400), size);
    font.setSubpixel(true);
    SkPaint ink;
    ink.setAntiAlias(true);
    ink.setColor4f({1.f, 0.93f, 0.9f, 1.f});
    float margin = size * 1.5f, line = size * 1.35f, y = margin + size;
    size_t cols = (size_t)std::max(20.f, ((float)w - 2 * margin) / (size * 0.6f));
    std::vector<std::string> lines = {"SCRIPT ERROR"};
    for (const ScriptError& er : errors) {
        std::string head = er.file.empty() ? std::string()
                                           : er.file + (er.line > 0 ? ":" + std::to_string(er.line) : "");
        if (!head.empty()) lines.push_back(head);
        size_t start = 0;
        while (start < er.message.size()) {
            size_t nl = er.message.find('\n', start);
            std::string l = er.message.substr(start, nl == std::string::npos ? std::string::npos
                                                                             : nl - start);
            for (size_t i = 0; i < l.size() || i == 0; i += cols) lines.push_back(l.substr(i, cols));
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
        lines.push_back("");
    }
    for (const std::string& l : lines) {
        if (y > (float)h - margin) break;
        c->drawSimpleText(l.data(), l.size(), SkTextEncoding::kUTF8, margin, y, font, ink);
        y += line;
    }
}

}  // namespace

unsigned script_clip_texture(const AppState& state, const Clip& clip, const std::string& key,
                             float t, int canvas_w, int canvas_h, int surface_w, int surface_h,
                             bool exporting, std::vector<ScriptError>& errors) {
    if (canvas_w <= 0 || canvas_h <= 0 || surface_w <= 0 || surface_h <= 0) return 0;
    std::unique_ptr<Entry>& slot = g_entries[key];
    if (!slot) slot = std::make_unique<Entry>();
    Entry& e = *slot;
    uint64_t audio = script_audio_epoch(state) ^ script_words_epoch(state);
    // Scripts see the frame grid: t is quantised to the project frame so
    // preview, render_still and export agree exactly on event boundaries.
    const double fps = state.fps > 0 ? (double)state.fps : 30.0;
    const long frame = std::lround((double)t * fps);
    bool rebuild = !e.built || e.force_build || e.script_path != clip.script_path ||
                   e.script_params != clip.script_params || e.rt.poll_dirty();
    if (!rebuild && !e.faces_waiting && e.last_frame == frame && e.last_w == canvas_w &&
        e.last_h == canvas_h && e.last_sw == surface_w && e.last_sh == surface_h &&
        e.last_audio == audio && e.last_exporting == exporting) {
        errors.insert(errors.end(), e.errors.begin(), e.errors.end());
        return e.surface.texture();
    }
    e.last_frame = frame;
    e.last_w = canvas_w;
    e.last_h = canvas_h;
    e.last_sw = surface_w;
    e.last_sh = surface_h;
    e.last_audio = audio;
    e.last_exporting = exporting;
    e.faces_waiting = false;
    if (rebuild) {
        e.script_path = clip.script_path;
        e.script_params = clip.script_params;
        e.force_build = false;
        e.built = true;
        e.builds++;
        e.build_errors.clear();
        e.build_ok = e.rt.build(state, clip, canvas_w, canvas_h, e.build_errors);
    }

    ScriptFrame f;
    f.frame = (int)frame;
    f.fps = fps;
    f.t = (double)frame / fps;
    f.local = f.t - (double)clip.start;
    f.width = canvas_w;
    f.height = canvas_h;
    f.duration = (double)clip.end - (double)clip.start;
    f.exporting = exporting;

    std::string gerr;
    std::vector<ScriptError> errs;
    bool ok = e.build_ok;
    if (!ok) errs = e.build_errors;
    SkCanvas* c = nullptr;
    // Export renders with every face track it asks for: when a render
    // requested a track that is still building, block on it and render again.
    for (int pass = 0; ok; ++pass) {
        c = e.surface.begin(surface_w, surface_h, &gerr);
        if (!c) {
            errors.push_back(ScriptError{gerr, "", 0});
            return 0;
        }
        errs.clear();
        auto t0 = std::chrono::steady_clock::now();
        ok = e.rt.render(state, f, c, errs);
        e.render_ms = ms_since(t0);
        if (!ok) break;
        std::vector<std::string> missing;
        for (const std::string& path : e.rt.face_requests())
            if (face_cache_status(path, nullptr) != FaceCacheStatus::Ready) missing.push_back(path);
        if (missing.empty()) break;
        if (!exporting || pass > 0) {
            e.faces_waiting = true;
            break;
        }
        e.surface.end(std::string(), {}, canvas_w, canvas_h, nullptr);
        c = nullptr;
        for (const std::string& path : missing) {
            if (!face_cache_ensure_sync(path, 0, nullptr)) {
                errs.push_back(ScriptError{"face tracking failed for " + path, path, 0});
                ok = false;
            }
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    if (ok) {
        std::string perr;
        if (!e.surface.end(e.rt.post_frag(), e.rt.post_uniforms(), canvas_w, canvas_h, &perr))
            errs.push_back(ScriptError{"post shader: " + perr, "", 0});
    } else {
        if (!c) c = e.surface.begin(surface_w, surface_h, &gerr);
        if (!c) {
            errors.push_back(ScriptError{gerr, "", 0});
            return 0;
        }
        c->restoreToCount(1);
        c->resetMatrix();
        c->clear(SK_ColorTRANSPARENT);
        draw_error_card(c, surface_w, surface_h, errs);
        e.surface.end(std::string(), {}, canvas_w, canvas_h, nullptr);
    }
    e.flush_ms = ms_since(t1);
    if (errs.size() > kMaxErrors) errs.resize(kMaxErrors);
    e.errors = errs;
    errors.insert(errors.end(), e.errors.begin(), e.errors.end());
    return e.surface.texture();
}

void script_clip_gc(const AppState& state) {
    if (g_entries.empty()) return;
    for (auto it = g_entries.begin(); it != g_entries.end();) {
        int ti = -1, ci = -1;
        bool live = std::sscanf(it->first.c_str(), "%d:%d", &ti, &ci) == 2 && ti >= 0 && ci >= 0 &&
                    ti < (int)state.tracks.size() && ci < (int)state.tracks[ti].clips.size() &&
                    state.tracks[ti].clips[ci].clip_type == ClipType::Script;
        if (live) ++it;
        else it = g_entries.erase(it);
    }
}

void script_clip_invalidate(const std::string& key) {
    auto it = g_entries.find(key);
    if (it != g_entries.end()) it->second->force_build = true;
}

std::vector<ScriptClipReport> script_clip_reports(const AppState& state) {
    std::vector<ScriptClipReport> out;
    for (size_t ti = 0; ti < state.tracks.size(); ++ti) {
        for (size_t ci = 0; ci < state.tracks[ti].clips.size(); ++ci) {
            if (state.tracks[ti].clips[ci].clip_type != ClipType::Script) continue;
            std::string key = script_clip_key((int)ti, (int)ci);
            auto it = g_entries.find(key);
            if (it == g_entries.end()) continue;
            const Entry& e = *it->second;
            out.push_back({key, e.errors, e.rt.log_tail(), e.render_ms, e.flush_ms, e.builds});
        }
    }
    return out;
}

#else  // !PMS_HAS_GL — Script clips need the GL renderer (Skia on GL).

// script_runtime.cpp (QuickJS + Skia) is not built here, so no `pms:` module
// can be resolved: "" = unresolvable (script_runtime.h). lay_typography_script
// uses this to validate the preset id, so headless reports it as unknown rather
// than laying a layer that nothing can render.
std::string script_resolve_spec(const std::string&, const std::string&, const std::string&) {
    return {};
}

unsigned script_clip_texture(const AppState&, const Clip&, const std::string&, float, int, int,
                             int, int, bool, std::vector<ScriptError>&) {
    return 0;
}
void script_clip_gc(const AppState&) {}
void script_clip_invalidate(const std::string&) {}
std::vector<ScriptClipReport> script_clip_reports(const AppState&) { return {}; }

#endif
