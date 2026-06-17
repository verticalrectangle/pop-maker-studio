#include "project.h"
#include "ui/timeline.h"
#include "body_fx.h"
#include "ui/panel_media.h"  // bin_backfill_from_timeline
#include <algorithm>
#include <fstream>
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>

// ── Binary serialization helpers ──────────────────────────────────────────────

static const uint32_t MAGIC   = 0x534D5001u; // "PMS\x01"
static const uint32_t VERSION = 52u;  // v52: persist voice-conversion result (status/out/model)

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
    // v11: datamosh + glitch corruption + chroma key brick
    w.pod(c.fx_glitch_corruption); w.pod(c.fx_glitch_corruption_bleed);
    w.pod(c.fx_datamosh_intensity);
    w.pod(0.08f);   // was datamosh_decay  (removed)
    w.pod((int)16); // was datamosh_block_size (removed)
    w.pod(0.0f);    // was datamosh_bleedback (removed)
    w.pod(c.fx_chroma_key_r); w.pod(c.fx_chroma_key_g); w.pod(c.fx_chroma_key_b);
    w.pod(c.fx_chroma_key_threshold); w.pod(c.fx_chroma_key_softness);
    // v12: remove background
    w.pod((uint8_t)c.bg_remove_on); w.pod(c.bg_remove_softness);
    w.str(c.bg_remove_mask_dir);
    // v13: text position X + wrap width
    w.pod(c.sub_pos_x); w.pod(c.sub_wrap_w);
    // v14: per-clip font size
    w.pod(c.font_size);
    // v15: datamosh spread
    w.pod(c.fx_datamosh_spread);
    // v16-v22: generated effects
#include "generated/fx_project_write.h"
    // v24: background clip fields
    w.pod(c.bg_speed); w.pod(c.bg_intensity);
    for (int i=0;i<4;++i) w.pod(c.bg_c1[i]);
    for (int i=0;i<4;++i) w.pod(c.bg_c2[i]);
    for (int i=0;i<4;++i) w.pod(c.bg_c3[i]);
    // v23: per-clip beat sync fields
    w.pod(c.beat_src_track); w.pod(c.beat_src_clip); w.pod(c.beat_decay);
    w.pod(c.beat_bpm);
    w.str(c.beats_json_path);
    uint32_t nb = (uint32_t)c.beats.size();
    w.pod(nb);
    for (float b : c.beats) w.pod(b);
    // ktracks
    uint32_t nk = (uint32_t)c.ktracks.size();
    w.pod(nk);
    for (auto& [key, pt] : c.ktracks) { w.str(key); write_proptrack(w, pt); }
    // per-clip word list
    uint32_t nw = (uint32_t)c.words.size();
    w.pod(nw);
    for (auto& we : c.words) write_wordentry(w, we);
    // v26: runtime FX
    w.str(c.runtime_fx_id);
    w.vecf(c.runtime_fx_params);
    w.pod(c.runtime_fx_amount);
    // v27: body FX
    w.pod((int)c.body_fx_type);
    for (int i=0;i<4;++i) w.pod(c.body_fx_params[i]);
    w.pod(c.body_fx_amount);
    // v28: text style
    w.pod((uint8_t)c.ts.shadow_enabled); w.pod(c.ts.shadow_ox); w.pod(c.ts.shadow_oy);
    for (int i=0;i<4;++i) w.pod(c.ts.shadow_col[i]);
    w.pod((uint8_t)c.ts.stroke_enabled); w.pod(c.ts.stroke_w);
    for (int i=0;i<4;++i) w.pod(c.ts.stroke_col[i]);
    w.pod((uint8_t)c.ts.glow_enabled); w.pod(c.ts.glow_r);
    for (int i=0;i<4;++i) w.pod(c.ts.glow_col[i]);
    w.pod((uint8_t)c.ts.bg_enabled);
    for (int i=0;i<4;++i) w.pod(c.ts.bg_col[i]);
    w.pod(c.ts.bg_pad_x); w.pod(c.ts.bg_pad_y); w.pod(c.ts.bg_corner);
    // v30: MultiFX chain
    w.pod(c.rel_start); w.pod(c.rel_end);
    uint32_t nchain = (uint32_t)c.fx_chain.size();
    w.pod(nchain);
    for (auto& se : c.fx_chain) write_clip(w, se);
    // v31: callout overlay fields
    w.pod(c.callout_style); w.pod((uint8_t)c.callout_arrow);
    w.pod(c.arrow_tx); w.pod(c.arrow_ty);
    // v32–33: body_fx_mask_status/progress/error were written here; removed in v34
    // v35: per-clip color grade
    w.pod(c.grade_brightness); w.pod(c.grade_contrast);
    w.pod(c.grade_saturation); w.pod(c.grade_hue);
    // v37: non-destructive crop
    w.pod(c.crop_l); w.pod(c.crop_t); w.pod(c.crop_r); w.pod(c.crop_b);
    // v38: record brick takes
    uint32_t ntk = (uint32_t)c.rec_takes.size();
    w.pod(ntk);
    for (auto& tp : c.rec_takes) w.str(tp);
    w.pod(c.rec_take_sel);
    // v39: per-clip AudioFX (was never persisted — voice model, transpose,
    // autotune, delay, reverb all reset on reload before this)
    {
        const AudioFX& fx = c.audio_fx;
        w.pod((uint8_t)fx.autotune_on); w.pod(fx.autotune_key);
        w.pod(fx.autotune_scale);       w.pod(fx.autotune_speed);
        w.pod((uint8_t)fx.pitch_on);    w.pod(fx.pitch_semitones);
        w.pod((uint8_t)fx.formant_on);  w.pod(fx.formant_shift);
        w.pod((uint8_t)fx.delay_on);    w.pod(fx.delay_time);
        w.pod(fx.delay_feedback);       w.pod(fx.delay_mix);
        w.pod((uint8_t)fx.reverb_on);   w.pod(fx.reverb_room);
        w.pod(fx.reverb_damp);          w.pod(fx.reverb_mix);
        w.pod((uint8_t)fx.voice_convert_on);
        w.str(fx.voice_model_path);
        w.pod((uint8_t)fx.voice_pitch_auto);
        w.pod(fx.voice_pitch_semitones);
        w.pod(fx.mix);   // v51: brick dry/wet
    }
    // v40: FX-brick coupling
    w.pod((uint8_t)c.fx_coupled);
    w.str(c.fx_host_sid);
    // v43: face filter
    w.pod(c.face_filter);
    w.pod(c.face_filter_amt);
    // v45: non-destructive letter case (render-time; the typed case is preserved)
    w.pod(c.text_case);
    // v46: frame-rate conform (src_fps = native rate; flags drive blend / loop)
    w.pod(c.src_fps);
    w.pod((uint8_t)c.conform_smooth);
    w.pod((uint8_t)c.clip_loop);
    // v48: Capture IMG brick (photo-mode camera brick — takes are stills)
    w.pod((uint8_t)c.rec_photo);
    // v49: A/V camera capture (record mic + manual sync offset)
    w.pod((uint8_t)c.rec_audio);
    w.pod(c.rec_av_offset_ms);
    // v50: typography face + kinetic/styling params
    w.str(c.sub_font);
    w.pod(c.ease);
    w.pod(c.tracking);
    w.pod(c.anim_unit);
    w.pod(c.anim_stagger);
    w.pod(c.karaoke_mode);
    w.pod(c.grad_mode);
    w.pod(c.grad_col2[0]); w.pod(c.grad_col2[1]); w.pod(c.grad_col2[2]); w.pod(c.grad_col2[3]);
    // v52: voice-conversion RESULT (so a converted take survives a reload — the
    // FX settings were already saved, but the Ready status + output path weren't,
    // so playback/export fell back to the original on load). Transient fields
    // (progress, error) aren't persisted.
    w.pod((int32_t)c.vc_status);
    w.str(c.vc_out_path);
    w.str(c.vc_model_used);
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
    if (version >= 11u) {
        c.fx_glitch_corruption      = r.pod<float>();
        c.fx_glitch_corruption_bleed= r.pod<float>();
        c.fx_datamosh_intensity = r.pod<float>();
        r.pod<float>();  // was datamosh_decay (removed)
        r.pod<int>();    // was datamosh_block_size (removed)
        r.pod<float>();  // was datamosh_bleedback (removed)
        c.fx_chroma_key_r           = r.pod<float>();
        c.fx_chroma_key_g           = r.pod<float>();
        c.fx_chroma_key_b           = r.pod<float>();
        c.fx_chroma_key_threshold   = r.pod<float>();
        c.fx_chroma_key_softness    = r.pod<float>();
    }
    if (version >= 12u) {
        c.bg_remove_on       = (bool)r.pod<uint8_t>();
        c.bg_remove_softness = r.pod<float>();
        c.bg_remove_mask_dir = r.str();
        if (c.bg_remove_on && !c.bg_remove_mask_dir.empty()) {
            namespace fs = std::filesystem;
            c.bg_remove_status = fs::exists(c.bg_remove_mask_dir + "/fps.txt")
                                  ? BgRemoveStatus::Ready : BgRemoveStatus::Idle;
        }
    }
    if (version >= 13u) {
        c.sub_pos_x  = r.pod<float>();
        c.sub_wrap_w = r.pod<float>();
    }
    if (version >= 14u) {
        c.font_size = r.pod<float>();
    }
    if (version >= 15u) {
        c.fx_datamosh_spread = r.pod<float>();
    }
#include "generated/fx_project_read.h"
    if (version >= 24u) {
        c.bg_speed     = r.pod<float>(); c.bg_intensity = r.pod<float>();
        for (int i=0;i<4;++i) c.bg_c1[i] = r.pod<float>();
        for (int i=0;i<4;++i) c.bg_c2[i] = r.pod<float>();
        for (int i=0;i<4;++i) c.bg_c3[i] = r.pod<float>();
    }
    if (version >= 23u) {
        c.beat_src_track = r.pod<int>(); c.beat_src_clip = r.pod<int>(); c.beat_decay = r.pod<float>();
        c.beat_bpm = r.pod<float>();
        c.beats_json_path = r.str();
        uint32_t nb = r.pod<uint32_t>();
        c.beats.reserve(nb);
        for (uint32_t i = 0; i < nb && r.ok; ++i) c.beats.push_back(r.pod<float>());
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
    // v26: runtime FX
    if (version >= 26u) {
        c.runtime_fx_id     = r.str();
        c.runtime_fx_params = r.vecf();
        c.runtime_fx_amount = r.pod<float>();
    }
    // v27: body FX  (v34+: RemoveBackground inserted at index 0, shift old values by +1)
    if (version >= 27u) {
        int raw_type = r.pod<int>();
        if (version < 34u) raw_type += 1;  // remap: old NeonOutline=0 → new NeonOutline=1
        c.body_fx_type = (BodyFXType)raw_type;
        for (int i=0;i<4;++i) c.body_fx_params[i] = r.pod<float>();
        c.body_fx_amount = r.pod<float>();
    }
    // v28: text style (default-constructed c.ts already gives backward-compatible shadow)
    if (version >= 28u) {
        c.ts.shadow_enabled = (bool)r.pod<uint8_t>();
        c.ts.shadow_ox = r.pod<float>(); c.ts.shadow_oy = r.pod<float>();
        for (int i=0;i<4;++i) c.ts.shadow_col[i] = r.pod<float>();
        c.ts.stroke_enabled = (bool)r.pod<uint8_t>(); c.ts.stroke_w = r.pod<float>();
        for (int i=0;i<4;++i) c.ts.stroke_col[i] = r.pod<float>();
        c.ts.glow_enabled = (bool)r.pod<uint8_t>(); c.ts.glow_r = r.pod<float>();
        for (int i=0;i<4;++i) c.ts.glow_col[i] = r.pod<float>();
        c.ts.bg_enabled = (bool)r.pod<uint8_t>();
        for (int i=0;i<4;++i) c.ts.bg_col[i] = r.pod<float>();
        c.ts.bg_pad_x = r.pod<float>(); c.ts.bg_pad_y = r.pod<float>(); c.ts.bg_corner = r.pod<float>();
    }
    // v30: MultiFX chain
    if (version >= 30u) {
        c.rel_start = r.pod<float>(); c.rel_end = r.pod<float>();
        uint32_t nchain = r.pod<uint32_t>();
        c.fx_chain.reserve(nchain);
        for (uint32_t i = 0; i < nchain && r.ok; ++i)
            c.fx_chain.push_back(read_clip(r, version));
    }
    // v31: callout overlay fields
    if (version >= 31u) {
        c.callout_style = r.pod<int>();
        c.callout_arrow = (bool)r.pod<uint8_t>();
        c.arrow_tx      = r.pod<float>();
        c.arrow_ty      = r.pod<float>();
    }
    // v32–33: body_fx_mask_status/progress/error — read and discard (removed in v34)
    if (version >= 32u && version < 34u) {
        r.pod<uint8_t>(); r.pod<float>(); r.str();
    }
    if (version >= 35u) {
        c.grade_brightness = r.pod<float>(); c.grade_contrast   = r.pod<float>();
        c.grade_saturation = r.pod<float>(); c.grade_hue        = r.pod<float>();
    }
    if (version >= 37u) {
        c.crop_l = r.pod<float>(); c.crop_t = r.pod<float>();
        c.crop_r = r.pod<float>(); c.crop_b = r.pod<float>();
    }
    if (version >= 38u) {
        uint32_t ntk = r.pod<uint32_t>();
        c.rec_takes.reserve(ntk);
        for (uint32_t i = 0; i < ntk && r.ok; ++i) c.rec_takes.push_back(r.str());
        c.rec_take_sel = r.pod<int>();
        if (c.rec_take_sel >= (int)c.rec_takes.size()) c.rec_take_sel = -1;
        // Drop takes whose files vanished (managed dir cleanup etc.).
        if (!c.rec_takes.empty()) {
            namespace fs = std::filesystem;
            std::string sel = (c.rec_take_sel >= 0) ? c.rec_takes[c.rec_take_sel] : "";
            c.rec_takes.erase(
                std::remove_if(c.rec_takes.begin(), c.rec_takes.end(),
                               [](const std::string& p) { return !fs::exists(p); }),
                c.rec_takes.end());
            c.rec_take_sel = -1;
            for (int i = 0; i < (int)c.rec_takes.size(); ++i)
                if (c.rec_takes[i] == sel) { c.rec_take_sel = i; break; }
            if (c.rec_take_sel < 0 && !c.rec_takes.empty())
                c.rec_take_sel = (int)c.rec_takes.size() - 1;
        }
        // VideoRecord bricks mirror the selected take into text (video
        // draw/export paths consume clip.text).
        if (c.clip_type == ClipType::VideoRecord)
            c.text = (c.rec_take_sel >= 0 &&
                      c.rec_take_sel < (int)c.rec_takes.size())
                     ? c.rec_takes[(size_t)c.rec_take_sel] : "";
    }
    if (version >= 39u) {
        AudioFX& fx = c.audio_fx;
        fx.autotune_on      = (bool)r.pod<uint8_t>();
        fx.autotune_key     = r.pod<int>();
        fx.autotune_scale   = r.pod<int>();
        fx.autotune_speed   = r.pod<float>();
        fx.pitch_on         = (bool)r.pod<uint8_t>();
        fx.pitch_semitones  = r.pod<float>();
        fx.formant_on       = (bool)r.pod<uint8_t>();
        fx.formant_shift    = r.pod<float>();
        fx.delay_on         = (bool)r.pod<uint8_t>();
        fx.delay_time       = r.pod<float>();
        fx.delay_feedback   = r.pod<float>();
        fx.delay_mix        = r.pod<float>();
        fx.reverb_on        = (bool)r.pod<uint8_t>();
        fx.reverb_room      = r.pod<float>();
        fx.reverb_damp      = r.pod<float>();
        fx.reverb_mix       = r.pod<float>();
        fx.voice_convert_on = (bool)r.pod<uint8_t>();
        fx.voice_model_path = r.str();
        fx.voice_pitch_auto = (bool)r.pod<uint8_t>();
        fx.voice_pitch_semitones = r.pod<int>();
        if (version >= 51u) fx.mix = r.pod<float>();
        // Model file may have been deleted from the cache since saving
        if (!fx.voice_model_path.empty() &&
            !std::filesystem::exists(fx.voice_model_path)) {
            fx.voice_model_path.clear();
            fx.voice_convert_on = false;
        }
    }
    if (version >= 40u) {
        c.fx_coupled  = (bool)r.pod<uint8_t>();
        c.fx_host_sid = r.str();
    }
    if (version >= 43u) {
        c.face_filter     = r.pod<int>();
        c.face_filter_amt = r.pod<float>();
    }
    if (version >= 45u) c.text_case = r.pod<int>();
    if (version >= 46u) {
        c.src_fps        = r.pod<float>();
        c.conform_smooth = (bool)r.pod<uint8_t>();
        c.clip_loop      = (bool)r.pod<uint8_t>();
    }
    if (version >= 48u) c.rec_photo = (bool)r.pod<uint8_t>();
    if (version >= 49u) {
        c.rec_audio        = (bool)r.pod<uint8_t>();
        c.rec_av_offset_ms = r.pod<float>();
    }
    if (version >= 50u) {
        c.sub_font     = r.str();
        c.ease         = r.pod<int>();
        c.tracking     = r.pod<float>();
        c.anim_unit    = r.pod<int>();
        c.anim_stagger = r.pod<float>();
        c.karaoke_mode = r.pod<int>();
        c.grad_mode    = r.pod<int>();
        c.grad_col2[0] = r.pod<float>(); c.grad_col2[1] = r.pod<float>();
        c.grad_col2[2] = r.pod<float>(); c.grad_col2[3] = r.pod<float>();
    }
    if (version >= 52u) {
        c.vc_status     = (VcStatus)r.pod<int32_t>();
        c.vc_out_path   = r.str();
        c.vc_model_used = r.str();
        // A Processing status can't survive a reload (the job is gone); settle it
        // to Ready if the output exists, else Idle so it can be re-run.
        if (c.vc_status == VcStatus::Processing)
            c.vc_status = (!c.vc_out_path.empty() && std::filesystem::exists(c.vc_out_path))
                          ? VcStatus::Ready : VcStatus::Idle;
    }
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
    if (version >= 42u && version < 47u) (void)r.pod<int>();  // old track.bus — discard
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
    w.str(state.video_path);
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
    w.pod((uint8_t)AnimStyle::None); w.pod(state.font_weight);
    w.pod((uint8_t)state.format);

    // Subtitle
    w.pod((uint8_t)state.subtitle_mode); w.pod(state.subtitle_n);

    // Render settings
    w.pod(state.render_settings.crf); w.pod(state.render_settings.audio_bitrate);
    w.str(state.render_settings.preset); w.pod((uint8_t)state.render_settings.high_profile);
    w.pod((uint8_t)state.render_settings.use_vaapi);

    // UI
    w.pod(state.panel_tab);

    // Project settings (v5)
    w.pod(state.fps);

    // v31: chapter markers
    uint32_t nm = (uint32_t)state.markers.size();
    w.pod(nm);
    for (auto& m : state.markers) {
        w.pod(m.time);
        w.str(m.label);
        w.pod(m.color);
        w.pod((uint8_t)m.chapter);   // v44
    }

    // v33: lyrics word edits
    w.pod((uint32_t)state.lyrics_edits.size());
    for (auto& [k, v] : state.lyrics_edits) {
        w.pod(k);
        w.str(v);
    }

    // v36: project bin
    w.pod((uint32_t)state.bin.size());
    for (auto& p : state.bin) w.str(p);

    // v47: global buses removed (bus bricks are plain clips). Nothing written
    // here anymore; the loader skips the old block for versions 42..46.

    // v44: loop brace region (armed state + in/out)
    w.pod((uint8_t)state.loop_play);
    w.pod(state.loop_in);
    w.pod(state.loop_out);

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
    state.video_path         = r.str();
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
    r.pod<uint8_t>(); state.style = AnimStyle::None; // project-level anim style is always None; presets own it
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
    if (version >= 29u)
        state.render_settings.use_vaapi = (bool)r.pod<uint8_t>();

    // UI
    state.panel_tab = r.pod<int>();

    // Project settings (v5)
    if (version >= 5u) state.fps = r.pod<int>();

    // v31: chapter markers
    if (version >= 31u) {
        uint32_t nm = r.pod<uint32_t>();
        state.markers.reserve(nm);
        for (uint32_t i = 0; i < nm && r.ok; ++i) {
            Marker m;
            m.time  = r.pod<float>();
            m.label = r.str();
            m.color = r.pod<uint32_t>();
            // Pre-v44 markers were all "chapter markers"; preserve that intent.
            m.chapter = (version >= 44u) ? (bool)r.pod<uint8_t>() : true;
            state.markers.push_back(std::move(m));
        }
    }

    // v33: lyrics word edits
    if (version >= 33u) {
        uint32_t ne = r.pod<uint32_t>();
        for (uint32_t i = 0; i < ne && r.ok; ++i) {
            int k = r.pod<int>();
            std::string v = r.str();
            state.lyrics_edits[k] = std::move(v);
        }
    }

    // v36: project bin
    if (version >= 36u) {
        uint32_t nb = r.pod<uint32_t>();
        state.bin.reserve(nb);
        for (uint32_t i = 0; i < nb && r.ok; ++i)
            state.bin.push_back(r.str());
    }
    // v42: audio buses
    // v42..46: old global buses block — read and discard to keep byte alignment
    // (bus bricks are now plain clips, already loaded as part of the tracks).
    if (version >= 42u && version < 47u) {
        uint32_t nbus = r.pod<uint32_t>();
        for (uint32_t i = 0; i < nbus && r.ok; ++i) {
            (void)r.str();           // name
            (void)r.pod<float>();    // gain
            uint32_t nch = r.pod<uint32_t>();
            for (uint32_t k = 0; k < nch && r.ok; ++k)
                (void)read_clip(r, version);   // discard fx-chain entries
        }
    }

    // v44: loop brace region
    if (version >= 44u) {
        state.loop_play = (bool)r.pod<uint8_t>();
        state.loop_in   = r.pod<float>();
        state.loop_out  = r.pod<float>();
    }

    // Backfill for pre-v36 projects: derive the bin from existing clip paths
    // so users opening older projects still see their media in the bin.
    bin_backfill_from_timeline(state);

    // Pre-v40 migration: overlap-glass was implicit — every video FX brick
    // sitting on content becomes a coupled Video Multi-FX chain (stacked
    // bricks merge into one per host, stacking order preserved). Same couple
    // semantics as the timeline's 1.5 s ring, minus the wait.
    if (r.ok && version < 41u) {
        for (int ti = 0; ti < (int)state.tracks.size(); ++ti) {
            auto& clips = state.tracks[ti].clips;
            for (int ci = 0; ci < (int)clips.size(); ) {
                Clip& c = clips[(size_t)ci];
                bool fx_kind = c.clip_type == ClipType::Effect ||
                               c.clip_type == ClipType::MultiFX ||
                               c.clip_type == ClipType::BodyFX ||
                               c.clip_type == ClipType::AudioMultiFX;
                bool audio_kind = fx_brick_is_audio_kind(c);
                if (fx_kind && !c.fx_coupled &&
                    (audio_kind || fx_brick_is_video(c))) {
                    int best = -1; float bov = 0.05f;
                    for (int k = 0; k < (int)clips.size(); ++k) {
                        const Clip& hc = clips[(size_t)k];
                        bool hostable = audio_kind
                            ? (hc.clip_type == ClipType::Audio ||
                               hc.clip_type == ClipType::Record ||
                               hc.clip_type == ClipType::Video ||
                               hc.clip_type == ClipType::VideoRecord)
                            : (clip_is_videolike_type(hc.clip_type) ||
                               hc.clip_type == ClipType::Background);
                        if (!hostable) continue;
                        float ov = fminf(c.end, hc.end) - fmaxf(c.start, hc.start);
                        if (ov > bov) { bov = ov; best = k; }
                    }
                    if (best >= 0) {
                        int nci = timeline_couple_fx_brick(state, ti, ci, best);
                        // Merged-away brick shrinks the list — don't advance.
                        if (nci != ci) continue;
                    }
                }
                ++ci;
            }
        }
    }

    return r.ok;
}

// ── Collect: produce a self-contained project folder ──────────────────────────
bool collect_project(AppState& state, const std::string& pms_path,
                     std::string& err, int* copied) {
    namespace fs = std::filesystem;
    std::error_code ec;

    fs::path dest_pms(pms_path);
    fs::path dest_dir = dest_pms.parent_path();
    fs::path media_dir = dest_dir / "media";
    fs::create_directories(media_dir, ec);
    if (ec) { err = "could not create " + media_dir.string(); return false; }
    std::string media_canon = fs::weakly_canonical(media_dir, ec).string();

    std::map<std::string, std::string> remap;   // original abs path -> collected copy
    std::set<std::string>              used;     // destination filenames in use
    int n_copied = 0;

    // Copy one referenced file into media/ and rewrite the reference in place.
    // A reference that isn't an existing regular file (literal text, a preset
    // token, an empty string) is left untouched.
    auto collect_one = [&](std::string& ref) {
        if (ref.empty()) return;
        fs::path src(ref);
        std::error_code e;
        if (!fs::exists(src, e) || !fs::is_regular_file(src, e)) return;
        std::string srcabs = fs::weakly_canonical(src, e).string();
        if (srcabs.empty()) srcabs = src.string();

        // Already inside this project's media folder → nothing to do.
        if (!media_canon.empty() && srcabs.rfind(media_canon, 0) == 0) return;

        auto it = remap.find(srcabs);
        if (it != remap.end()) { ref = it->second; return; }

        // Unique destination name (dedup basename collisions across sources).
        std::string base = src.filename().string();
        std::string name = base;
        for (int k = 1; used.count(name); ++k) {
            std::string stem = fs::path(base).stem().string();
            std::string ext  = fs::path(base).extension().string();
            name = stem + "_" + std::to_string(k) + ext;
        }
        fs::path dst = media_dir / name;
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, e);
        if (e) return;   // copy failed: leave the reference pointing at the original
        used.insert(name);
        std::string dstabs = dst.string();
        remap[srcabs] = dstabs;
        ref = dstabs;
        ++n_copied;
    };

    for (auto& tr : state.tracks)
        for (auto& cl : tr.clips) {
            collect_one(cl.text);
            collect_one(cl.source_id);
            collect_one(cl.fx_lut_path);
        }
    collect_one(state.audio_path);
    for (auto& b : state.bin) collect_one(b);

    state.project_path = pms_path;
    if (!project_save(state, pms_path)) { err = "could not save " + pms_path; return false; }
    if (copied) *copied = n_copied;
    return true;
}
