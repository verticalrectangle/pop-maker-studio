#include "project.h"
#include <fstream>
#include <cstring>
#include <cstdint>

// ── Binary serialization helpers ──────────────────────────────────────────────

static const uint32_t MAGIC   = 0x534D5001u; // "PMS\x01"
static const uint32_t VERSION = 10u;

struct Writer {
    std::ofstream f;
    bool ok = true;
    explicit Writer(const std::string& path) : f(path, std::ios::binary) { ok = f.good(); }
    template<typename T> void pod(const T& v) {
        if (!ok) return;
        f.write(reinterpret_cast<const char*>(&v), sizeof(T));
        ok = f.good();
    }
    void str(const std::string& s) {
        uint32_t n = (uint32_t)s.size();
        pod(n);
        if (n) { f.write(s.data(), n); ok = f.good(); }
    }
    void vecf(const std::vector<float>& v) {
        uint32_t n = (uint32_t)v.size();
        pod(n);
        if (n) { f.write(reinterpret_cast<const char*>(v.data()), n*sizeof(float)); ok = f.good(); }
    }
};

struct Reader {
    std::ifstream f;
    bool ok = true;
    explicit Reader(const std::string& path) : f(path, std::ios::binary) { ok = f.good(); }
    template<typename T> T pod() {
        T v{}; if (!ok) return v;
        f.read(reinterpret_cast<char*>(&v), sizeof(T));
        ok = f.good();
        return v;
    }
    std::string str() {
        uint32_t n = pod<uint32_t>();
        if (!ok || n > (1u<<24)) { ok=false; return {}; }
        std::string s(n, '\0');
        if (n) { f.read(s.data(), n); ok = f.good(); }
        return s;
    }
    std::vector<float> vecf() {
        uint32_t n = pod<uint32_t>();
        if (!ok || n > (1u<<24)) { ok=false; return {}; }
        std::vector<float> v(n);
        if (n) { f.read(reinterpret_cast<char*>(v.data()), n*sizeof(float)); ok = f.good(); }
        return v;
    }
};

// ── Keyframe / PropTrack ──────────────────────────────────────────────────────

static void write_keyframe(Writer& w, const Keyframe& k) {
    w.pod(k.time); w.pod(k.value); w.pod((uint8_t)k.interp);
}
static Keyframe read_keyframe(Reader& r) {
    Keyframe k;
    k.time  = r.pod<float>();
    k.value = r.pod<float>();
    k.interp = (InterpType)r.pod<uint8_t>();
    return k;
}
static void write_proptrack(Writer& w, const PropTrack& pt) {
    uint32_t n = (uint32_t)pt.keys.size();
    w.pod(n);
    for (auto& k : pt.keys) write_keyframe(w, k);
}
static PropTrack read_proptrack(Reader& r) {
    PropTrack pt;
    uint32_t n = r.pod<uint32_t>();
    for (uint32_t i = 0; i < n && r.ok; ++i)
        pt.keys.push_back(read_keyframe(r));
    return pt;
}

// ── Clip ──────────────────────────────────────────────────────────────────────

static void write_wordentry(Writer& w, const WordEntry& we) {
    w.str(we.text); w.pod(we.start); w.pod(we.end);
}
static WordEntry read_wordentry(Reader& r) {
    WordEntry we; we.text = r.str(); we.start = r.pod<float>(); we.end = r.pod<float>();
    return we;
}

static void write_clip(Writer& w, const Clip& c) {
    w.pod((uint8_t)c.clip_type);
    w.pod(c.start); w.pod(c.end);
    w.str(c.text); w.str(c.source_id);
    w.pod(c.volume); w.pod(c.speed); w.pod(c.opacity); w.pod(c.transition_pre);
    w.pod(c.fade_in); w.pod(c.fade_out);
    w.pod(c.in_point); w.pod(c.out_point);
    w.pod(c.pan); w.pod(c.blend_mode);
    w.pod((uint8_t)c.karaoke);
    w.pod(c.sub_pos); w.pod(c.sub_pos_y); w.pod(c.sub_anchor_h);
    w.pod(c.sub_color[0]); w.pod(c.sub_color[1]); w.pod(c.sub_color[2]); w.pod(c.sub_color[3]);
    w.pod((uint8_t)c.sub_color_override);
    w.pod(c.karaoke_highlight_color[0]); w.pod(c.karaoke_highlight_color[1]);
    w.pod(c.karaoke_highlight_color[2]); w.pod(c.karaoke_highlight_color[3]);
    w.pod(c.pos_x); w.pod(c.pos_y); w.pod(c.scale_x); w.pod(c.scale_y); w.pod(c.rotation);
    w.pod((uint8_t)c.clip_style);
    w.pod((uint8_t)c.transition_type); w.pod(c.transition_post);
    // Effect fields
    w.pod((uint8_t)c.fx_color_on); w.pod(c.fx_brightness); w.pod(c.fx_contrast);
    w.pod(c.fx_saturation); w.pod(c.fx_hue);
    w.pod((uint8_t)c.fx_blur_on); w.pod(c.fx_blur);
    w.pod((uint8_t)c.fx_vignette_on); w.pod(c.fx_vignette);
    w.pod((uint8_t)c.fx_text_on); w.pod(c.fx_opacity_mul); w.pod(c.fx_scale_mul);
    w.pod((uint8_t)c.muted);
    // Creative FX (v10)
    w.pod((uint8_t)c.fx_type);
    w.pod(c.fx_glitch_chroma); w.pod(c.fx_glitch_jitter);
    w.pod(c.fx_zoom_strength); w.pod(c.fx_zoom_decay); w.pod(c.fx_zoom_shake);
    w.str(c.fx_lut_path);
    w.pod(c.fx_leak_intensity); w.pod(c.fx_leak_speed);
    w.pod(c.fx_vhs_noise); w.pod(c.fx_vhs_bleed); w.pod(c.fx_vhs_tracking);
    // ktracks
    uint32_t nk = (uint32_t)c.ktracks.size();
    w.pod(nk);
    for (auto& [key, pt] : c.ktracks) { w.str(key); write_proptrack(w, pt); }
    // per-clip word list
    uint32_t nw = (uint32_t)c.words.size();
    w.pod(nw);
    for (auto& we : c.words) write_wordentry(w, we);
}

static Clip read_clip(Reader& r, uint32_t version) {
    Clip c;
    c.clip_type  = (ClipType)r.pod<uint8_t>();
    c.start      = r.pod<float>();
    c.end        = r.pod<float>();
    c.text       = r.str(); c.source_id = r.str();
    c.volume     = r.pod<float>(); c.speed = r.pod<float>();
    c.opacity    = r.pod<float>(); c.transition_pre = r.pod<float>();
    c.fade_in    = r.pod<float>(); c.fade_out = r.pod<float>();
    c.in_point   = r.pod<float>(); c.out_point = r.pod<float>();
    c.pan        = r.pod<float>(); c.blend_mode = r.pod<int>();
    c.karaoke    = (bool)r.pod<uint8_t>();
    c.sub_pos    = r.pod<int>(); c.sub_pos_y = r.pod<float>(); c.sub_anchor_h = r.pod<int>();
    c.sub_color[0]=r.pod<float>(); c.sub_color[1]=r.pod<float>();
    c.sub_color[2]=r.pod<float>(); c.sub_color[3]=r.pod<float>();
    c.sub_color_override = (bool)r.pod<uint8_t>();
    c.karaoke_highlight_color[0]=r.pod<float>(); c.karaoke_highlight_color[1]=r.pod<float>();
    c.karaoke_highlight_color[2]=r.pod<float>(); c.karaoke_highlight_color[3]=r.pod<float>();
    c.pos_x      = r.pod<float>(); c.pos_y    = r.pod<float>();
    c.scale_x    = r.pod<float>(); c.scale_y  = r.pod<float>();
    c.rotation   = r.pod<float>();
    c.clip_style = (AnimStyle)r.pod<uint8_t>();
    if (version >= 6u) {
        c.transition_type = (TransitionType)r.pod<uint8_t>();
        if (version >= 7u) c.transition_post = r.pod<float>();
        else               c.transition_post = c.transition_pre; // v6: symmetric fallback
    }
    // Effect fields
    c.fx_color_on   = (bool)r.pod<uint8_t>(); c.fx_brightness = r.pod<float>();
    c.fx_contrast   = r.pod<float>(); c.fx_saturation = r.pod<float>();
    c.fx_hue        = r.pod<float>();
    c.fx_blur_on    = (bool)r.pod<uint8_t>(); c.fx_blur = r.pod<float>();
    c.fx_vignette_on= (bool)r.pod<uint8_t>(); c.fx_vignette = r.pod<float>();
    c.fx_text_on    = (bool)r.pod<uint8_t>();
    c.fx_opacity_mul= r.pod<float>(); c.fx_scale_mul = r.pod<float>();
    if (version >= 8u) c.muted = (bool)r.pod<uint8_t>();
    if (version >= 10u) {
        c.fx_type         = (FXType)r.pod<uint8_t>();
        c.fx_glitch_chroma = r.pod<float>(); c.fx_glitch_jitter  = r.pod<float>();
        c.fx_zoom_strength = r.pod<float>(); c.fx_zoom_decay      = r.pod<float>();
        c.fx_zoom_shake    = r.pod<float>();
        c.fx_lut_path      = r.str();
        c.fx_leak_intensity= r.pod<float>(); c.fx_leak_speed      = r.pod<float>();
        c.fx_vhs_noise     = r.pod<float>(); c.fx_vhs_bleed       = r.pod<float>();
        c.fx_vhs_tracking  = r.pod<float>();
    }
    // ktracks
    uint32_t nk = r.pod<uint32_t>();
    for (uint32_t i = 0; i < nk && r.ok; ++i) {
        std::string key = r.str();
        c.ktracks[key] = read_proptrack(r);
    }
    // per-clip word list
    uint32_t nw = r.pod<uint32_t>();
    for (uint32_t i = 0; i < nw && r.ok; ++i)
        c.words.push_back(read_wordentry(r));
    return c;
}

// ── Track ─────────────────────────────────────────────────────────────────────

static void write_track(Writer& w, const Track& t) {
    w.str(t.name);
    w.pod((uint8_t)t.visible); w.pod((uint8_t)t.muted); w.pod((uint8_t)t.locked); w.pod(t.sub_row);
    uint32_t nc = (uint32_t)t.clips.size();
    w.pod(nc);
    for (auto& c : t.clips) write_clip(w, c);
}

static Track read_track(Reader& r, uint32_t version) {
    Track t;
    t.name    = r.str();
    t.visible = (bool)r.pod<uint8_t>(); t.muted = (bool)r.pod<uint8_t>();
    if (version >= 9u) t.locked = (bool)r.pod<uint8_t>();
    t.sub_row = r.pod<int>();
    uint32_t nc = r.pod<uint32_t>();
    for (uint32_t i = 0; i < nc && r.ok; ++i)
        t.clips.push_back(read_clip(r, version));
    return t;
}

// ── Public API ────────────────────────────────────────────────────────────────

bool project_save(const AppState& state, const std::string& path) {
    Writer w(path);
    if (!w.ok) return false;

    w.pod(MAGIC); w.pod(VERSION);

    // Paths
    w.str(state.audio_path); w.str(state.vocals_path);
    w.str(state.words_json_path); w.str(state.segments_json_path);
    w.str(state.beats_json_path); w.str(state.envelope_json_path);
    w.str(state.video_path); w.str(state.python_path);
    w.str(state.out_mp4); w.str(state.out_wav); w.str(state.out_srt);

    // Beat / envelope data
    w.pod(state.beat_bpm);
    w.vecf(state.beats);
    w.pod(state.envelope_fps);
    w.vecf(state.amplitude_envelope);

    // Tracks
    uint32_t nt = (uint32_t)state.tracks.size();
    w.pod(nt);
    for (auto& t : state.tracks) write_track(w, t);

    // Selection / view
    w.pod(state.selected_track); w.pod(state.selected_clip);
    w.pod(state.playhead); w.pod(state.duration);
    w.pod(state.tl_scroll); w.pod(state.tl_zoom); w.pod(state.tl_v_scroll);
    w.pod(state.panel_w); w.pod(state.tl_h_frac);

    // Style
    w.pod((uint8_t)state.style); w.pod(state.font_weight);
    w.pod((uint8_t)state.format);

    // Subtitle
    w.pod((uint8_t)state.subtitle_mode); w.pod(state.subtitle_n);

    // Render settings
    w.pod(state.render_settings.crf); w.pod(state.render_settings.audio_bitrate);
    w.str(state.render_settings.preset); w.pod((uint8_t)state.render_settings.high_profile);

    // UI
    w.pod(state.panel_tab);

    // Project settings (v5)
    w.pod(state.fps);

    return w.ok;
}

bool project_load(AppState& state, const std::string& path) {
    Reader r(path);
    if (!r.ok) return false;

    uint32_t magic   = r.pod<uint32_t>();
    uint32_t version = r.pod<uint32_t>();
    if (magic != MAGIC || version < 1u || version > VERSION) return false;

    state = AppState{};
    state.splash_timer = 0.f;

    // Paths
    state.audio_path         = r.str(); state.vocals_path   = r.str();
    state.words_json_path    = r.str(); state.segments_json_path = r.str();
    state.beats_json_path    = r.str(); state.envelope_json_path = r.str();
    state.video_path         = r.str(); state.python_path   = r.str();
    state.out_mp4            = r.str(); state.out_wav        = r.str();
    state.out_srt            = r.str();

    // Beat / envelope
    state.beat_bpm             = r.pod<float>();
    state.beats                = r.vecf();
    state.envelope_fps         = r.pod<float>();
    state.amplitude_envelope   = r.vecf();

    // Tracks
    uint32_t nt = r.pod<uint32_t>();
    for (uint32_t i = 0; i < nt && r.ok; ++i)
        state.tracks.push_back(read_track(r, version));

    // Selection / view
    state.selected_track = r.pod<int>(); state.selected_clip = r.pod<int>();
    state.playhead       = r.pod<float>(); state.duration    = r.pod<float>();
    state.tl_scroll      = r.pod<float>(); state.tl_zoom     = r.pod<float>();
    state.tl_v_scroll    = r.pod<float>();
    state.panel_w        = r.pod<float>(); state.tl_h_frac   = r.pod<float>();

    // Style
    state.style       = (AnimStyle)r.pod<uint8_t>();
    state.font_weight = r.pod<int>();
    state.format      = (OutputFormat)r.pod<uint8_t>();

    // Subtitle
    state.subtitle_mode = (SubtitleMode)r.pod<uint8_t>();
    state.subtitle_n    = r.pod<int>();

    // Render settings
    state.render_settings.crf          = r.pod<int>();
    state.render_settings.audio_bitrate= r.pod<int>();
    state.render_settings.preset       = r.str();
    state.render_settings.high_profile = (bool)r.pod<uint8_t>();

    // UI
    state.panel_tab = r.pod<int>();

    // Project settings (v5)
    if (version >= 5u) state.fps = r.pod<int>();

    return r.ok;
}
