#include "app.h"
#include "history.h"
#include "paths.h"
#include "audio.h"
#include "video.h"
#include "transcribe.h"
#include "render.h"
#include "fx_shader.h"
#include "presets.h"
#include "runtime_fx.h"
#include "recorder.h"
#include "video_recorder.h"
#include "ipc_server.h"
#include "agent_harness.h"
#include "globals.h"
#include "ui/theme.h"
#include "ui/screens.h"
#include "ui/panel_terminal.h"
#include "ui/studio_shared.h"   // last_playable_time
#include <imgui.h>
#include <algorithm>
#include <chrono>
#include <cmath>

// ── PropTrack ─────────────────────────────────────────────────────────────────

static float apply_interp(float t, InterpType it) {
    switch (it) {
        case InterpType::EaseIn:   return t * t;
        case InterpType::EaseOut:  return t * (2.f - t);
        case InterpType::EaseBoth: return t < 0.5f ? 2.f*t*t : -1.f + (4.f - 2.f*t)*t;
        case InterpType::Hold:     return 0.f;
        default:                   return t;
    }
}

float PropTrack::eval(float t) const {
    if (keys.empty()) return 0.f;
    if ((int)keys.size() == 1 || t <= keys.front().time) return keys.front().value;
    if (t >= keys.back().time) return keys.back().value;
    for (int i = 0; i < (int)keys.size() - 1; ++i) {
        if (t >= keys[i].time && t < keys[i+1].time) {
            if (keys[i].interp == InterpType::Hold) return keys[i].value;
            float alpha = (t - keys[i].time) / (keys[i+1].time - keys[i].time);
            alpha = apply_interp(alpha, keys[i].interp);
            return keys[i].value + alpha * (keys[i+1].value - keys[i].value);
        }
    }
    return keys.back().value;
}

void PropTrack::set(float t, float v, InterpType it) {
    for (auto& k : keys) {
        if (fabsf(k.time - t) < 0.02f) { k.value = v; k.interp = it; return; }
    }
    keys.push_back({t, v, it});
    std::sort(keys.begin(), keys.end(),
              [](const Keyframe& a, const Keyframe& b){ return a.time < b.time; });
}

void PropTrack::remove_at(float t, float tol) {
    keys.erase(std::remove_if(keys.begin(), keys.end(),
        [&](const Keyframe& k){ return fabsf(k.time - t) < tol; }), keys.end());
}

int PropTrack::find_nearest(float t, float tol) const {
    int best = -1; float bd = tol;
    for (int i = 0; i < (int)keys.size(); ++i) {
        float d = fabsf(keys[i].time - t);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

// ── Clip::eval_prop ───────────────────────────────────────────────────────────

// Registry of keyframable float fields on Clip — the single source of truth for
// eval_prop's static fallback and the keyframe UI/render routing. Every entry is
// animatable: with keys it interpolates, without keys eval_prop returns the
// static field (so non-animated clips render exactly as before). `opacity` is
// handled separately because of its fade-envelope interaction.
const ClipKfField kClipKfFields[] = {
    // Transform / audio level
    {"pos_x", &Clip::pos_x},   {"pos_y", &Clip::pos_y},
    {"scale_x", &Clip::scale_x}, {"scale_y", &Clip::scale_y},
    {"rotation", &Clip::rotation}, {"volume", &Clip::volume}, {"pan", &Clip::pan},
    {"sub_pos_x", &Clip::sub_pos_x}, {"sub_pos_y", &Clip::sub_pos_y},
    {"sub_wrap_w", &Clip::sub_wrap_w}, {"font_size", &Clip::font_size},
    // Look / colour grade
    {"fx_brightness", &Clip::fx_brightness}, {"fx_contrast", &Clip::fx_contrast},
    {"fx_saturation", &Clip::fx_saturation}, {"fx_hue", &Clip::fx_hue},
    {"fx_blur", &Clip::fx_blur}, {"fx_vignette", &Clip::fx_vignette},
    {"fx_opacity_mul", &Clip::fx_opacity_mul}, {"fx_scale_mul", &Clip::fx_scale_mul},
    // Creative FX values
    {"fx_glitch_chroma", &Clip::fx_glitch_chroma},
    {"fx_glitch_jitter", &Clip::fx_glitch_jitter},
    {"fx_glitch_corruption", &Clip::fx_glitch_corruption},
    {"fx_glitch_corruption_bleed", &Clip::fx_glitch_corruption_bleed},
    {"fx_zoom_strength", &Clip::fx_zoom_strength}, {"fx_zoom_decay", &Clip::fx_zoom_decay},
    {"fx_zoom_shake", &Clip::fx_zoom_shake},
    {"fx_leak_intensity", &Clip::fx_leak_intensity}, {"fx_leak_speed", &Clip::fx_leak_speed},
    {"fx_vhs_noise", &Clip::fx_vhs_noise}, {"fx_vhs_bleed", &Clip::fx_vhs_bleed},
    {"fx_vhs_tracking", &Clip::fx_vhs_tracking},
    {"fx_datamosh_intensity", &Clip::fx_datamosh_intensity},
    {"fx_chroma_key_threshold", &Clip::fx_chroma_key_threshold},
    {"fx_chroma_key_softness", &Clip::fx_chroma_key_softness},
    // Body / runtime FX amount, fades, transitions
    {"body_fx_amount", &Clip::body_fx_amount},
    {"runtime_fx_amount", &Clip::runtime_fx_amount},
    {"face_filter_amt", &Clip::face_filter_amt},
    {"fade_in", &Clip::fade_in}, {"fade_out", &Clip::fade_out},
    {"transition_pre", &Clip::transition_pre}, {"transition_post", &Clip::transition_post},
    // Generated shader-FX packs: amount + every param (one row each).
#include "generated/fx_kf_fields.h"
};
const int kClipKfFieldCount = (int)(sizeof(kClipKfFields) / sizeof(kClipKfFields[0]));

float Clip::eval_prop(const std::string& name, float playhead) const {
    float t = playhead - start;
    auto it = ktracks.find(name);
    if (it != ktracks.end() && !it->second.empty())
        return it->second.eval(t);
    if (name == "opacity") {
        float base = opacity;
        float dur  = end - start;
        if (fade_in  > 0.f && t < fade_in)
            base *= (t / fade_in);
        if (fade_out > 0.f && dur > 0.f && t > dur - fade_out)
            base *= ((dur - t) / fade_out);
        return fmaxf(0.f, fminf(1.f, base));
    }
    for (int i = 0; i < kClipKfFieldCount; ++i)
        if (name == kClipKfFields[i].name) return this->*(kClipKfFields[i].f);
    return 0.f;
}

// ── Split / trim keyframe handling ────────────────────────────────────────────

Clip clip_split_at(Clip& cl, float cut) {
    float c = cut - cl.start;  // split point relative to clip start (timeline s)
    Clip right = cl;
    right.start    = cut;
    right.in_point = cl.in_point + c * cl.speed;
    cl.end = cut;

    // Key times are relative to clip.start, so the right half's keys must
    // shift by -c. Both halves get a synthesized boundary key holding the
    // value at the cut so neither side's animation changes visibly.
    for (auto& [name, pt] : cl.ktracks) {
        if (pt.empty()) continue;
        const std::vector<Keyframe> orig = pt.keys;
        float v_cut = pt.eval(c);

        // Interp of the segment the cut lands in — carried onto the right
        // half's boundary key so easing continues roughly as before.
        InterpType seg_it = InterpType::Linear;
        for (int i = (int)orig.size() - 1; i >= 0; --i)
            if (orig[i].time <= c) { seg_it = orig[i].interp; break; }

        std::vector<Keyframe> lk, rk;
        for (const auto& k : orig) {
            if (k.time <= c) lk.push_back(k);
            else             rk.push_back({k.time - c, k.value, k.interp});
        }
        if (!rk.empty() && (lk.empty() || lk.back().time < c - 1e-4f))
            lk.push_back({c, v_cut, InterpType::Linear});
        if (rk.empty() || rk.front().time > 1e-4f)
            rk.insert(rk.begin(), {0.f, v_cut, seg_it});

        pt.keys = std::move(lk);
        right.ktracks[name].keys = std::move(rk);
    }
    for (auto it = cl.ktracks.begin(); it != cl.ktracks.end(); )
        it = it->second.empty() ? cl.ktracks.erase(it) : std::next(it);
    for (auto it = right.ktracks.begin(); it != right.ktracks.end(); )
        it = it->second.empty() ? right.ktracks.erase(it) : std::next(it);
    return right;
}

void clip_keys_shift(Clip& cl, float dt) {
    if (dt == 0.f) return;
    for (auto& [name, pt] : cl.ktracks)
        for (auto& k : pt.keys)
            k.time += dt;
}

// ── AppState ──────────────────────────────────────────────────────────────────

std::vector<std::pair<int,int>> AppState::subtitle_clip_indices() const {
    std::vector<std::pair<int,int>> out;
    for (int ti = 0; ti < (int)tracks.size(); ++ti) {
        if (!tracks[ti].visible) continue;
        for (int ci = 0; ci < (int)tracks[ti].clips.size(); ++ci)
            if (tracks[ti].clips[ci].clip_type == ClipType::Text     ||
                tracks[ti].clips[ci].clip_type == ClipType::Lyrics   ||
                tracks[ti].clips[ci].clip_type == ClipType::Subtitle)
                out.push_back({ti, ci});
    }
    std::sort(out.begin(), out.end(), [&](auto& a, auto& b){
        return tracks[a.first].clips[a.second].start <
               tracks[b.first].clips[b.second].start;
    });
    return out;
}

// _t is the current playhead (absolute) so animated colour-grade params are
// evaluated per-frame; eval_prop returns the static field when a param has no
// keys, so non-animated clips are unchanged.
static void accum_effect_clip(EffectAccum& acc, const Clip& cl, float _t = 0.f) {
    if (cl.fx_color_on) {
        acc.brightness += cl.eval_prop("fx_brightness", _t);
        acc.contrast   *= cl.eval_prop("fx_contrast",   _t);
        acc.saturation *= cl.eval_prop("fx_saturation", _t);
        acc.hue        += cl.eval_prop("fx_hue",        _t);
        acc.any_color   = true;
    }
    if (cl.fx_blur_on) {
        acc.blur     += cl.eval_prop("fx_blur", _t);
        acc.any_blur  = true;
    }
    if (cl.fx_vignette_on) {
        acc.vignette     = fminf(1.f, acc.vignette + cl.eval_prop("fx_vignette", _t));
        acc.any_vignette = true;
    }
    if (cl.fx_text_on) {
        acc.opacity_mul *= cl.eval_prop("fx_opacity_mul", _t);
        acc.scale_mul   *= cl.eval_prop("fx_scale_mul",   _t);
        acc.any_text     = true;
    }
}

// Accumulate a single creative-FX clip into acc. _cl_beat_pulse must be
// pre-computed by the caller (pass 0.f for MultiFX sub-clips).
// _cl_t is the current playhead time (needed for transform effects' progress).
static void accum_creative_clip(CreativeFXAccum& acc, const Clip& cl, float _cl_beat_pulse, float _cl_t = 0.f) {
    if (cl.fx_type == FXType::Grade    ||
        cl.fx_type == FXType::Blur     ||
        cl.fx_type == FXType::Vignette) return;
    // Value params route through eval_prop(_cl_t) so a keyframed brick animates
    // per-frame (eval_prop returns the static field when un-keyed). Colour
    // components / spread aren't in the registry → read directly.
    switch (cl.fx_type) {
        case FXType::Glitch:
            acc.glitch_on         = true; acc.any_cfx = true;
            acc.glitch_chroma     = fmaxf(acc.glitch_chroma,     cl.eval_prop("fx_glitch_chroma", _cl_t));
            acc.glitch_jitter     = fmaxf(acc.glitch_jitter,     cl.eval_prop("fx_glitch_jitter", _cl_t));
            acc.glitch_corruption       = fmaxf(acc.glitch_corruption,       cl.eval_prop("fx_glitch_corruption", _cl_t));
            acc.glitch_corruption_bleed = fmaxf(acc.glitch_corruption_bleed, cl.eval_prop("fx_glitch_corruption_bleed", _cl_t));
            break;
        case FXType::ZoomPunch:
            acc.zoom_on       = true; acc.any_cfx = true;
            acc.zoom_strength = fmaxf(acc.zoom_strength, cl.eval_prop("fx_zoom_strength", _cl_t));
            acc.zoom_decay    = fmaxf(acc.zoom_decay,    cl.eval_prop("fx_zoom_decay", _cl_t));
            acc.zoom_shake    = fmaxf(acc.zoom_shake,    cl.eval_prop("fx_zoom_shake", _cl_t));
            acc.zoom_src_track = cl.beat_src_track;
            acc.zoom_src_clip  = cl.beat_src_clip;
            break;
        case FXType::LightLeak:
            acc.leak_on        = true; acc.any_cfx = true;
            acc.leak_intensity = fmaxf(acc.leak_intensity, cl.eval_prop("fx_leak_intensity", _cl_t));
            acc.leak_speed     = fmaxf(acc.leak_speed,     cl.eval_prop("fx_leak_speed", _cl_t));
            break;
        case FXType::VHS:
            acc.vhs_on       = true; acc.any_cfx = true;
            acc.vhs_noise    = fmaxf(acc.vhs_noise,    cl.eval_prop("fx_vhs_noise", _cl_t));
            acc.vhs_bleed    = fmaxf(acc.vhs_bleed,    cl.eval_prop("fx_vhs_bleed", _cl_t));
            acc.vhs_tracking = fmaxf(acc.vhs_tracking, cl.eval_prop("fx_vhs_tracking", _cl_t));
            break;
        case FXType::Datamosh:
            acc.datamosh_on        = true; acc.any_cfx = true;
            acc.datamosh_intensity = fmaxf(acc.datamosh_intensity, cl.eval_prop("fx_datamosh_intensity", _cl_t));
            acc.datamosh_spread    = fmaxf(acc.datamosh_spread,    cl.fx_datamosh_spread);
            break;
        case FXType::ChromaKey:
            acc.chroma_key_on        = true; acc.any_cfx = true;
            acc.chroma_key_r         = cl.fx_chroma_key_r;
            acc.chroma_key_g         = cl.fx_chroma_key_g;
            acc.chroma_key_b         = cl.fx_chroma_key_b;
            acc.chroma_key_threshold = cl.eval_prop("fx_chroma_key_threshold", _cl_t);
            acc.chroma_key_softness  = cl.eval_prop("fx_chroma_key_softness", _cl_t);
            break;
        default:
#include "generated/fx_collect_cases.h"
            break;
    }
}

bool fx_type_is_audio_fx(FXType ft);  // defined in ui/studio_shared.cpp

bool fx_brick_is_video(const Clip& c) {
    if (c.clip_type == ClipType::MultiFX || c.clip_type == ClipType::BodyFX) return true;
    return c.clip_type == ClipType::Effect && !fx_type_is_audio_fx(c.fx_type);
}

bool fx_brick_is_audio_kind(const Clip& c) {
    if (c.clip_type == ClipType::AudioMultiFX) return true;
    return c.clip_type == ClipType::Effect && fx_type_is_audio_fx(c.fx_type);
}

// Hosts an audio FX brick can couple to: anything that makes sound.
static bool fx_audio_host_type(ClipType t) {
    return t == ClipType::Audio || t == ClipType::Record ||
           t == ClipType::Video || t == ClipType::VideoRecord;
}

std::string fx_host_fingerprint(const Clip& host) {
    // Record bricks swap their text per selected take — use stable sentinels.
    if (host.clip_type == ClipType::VideoRecord) return "\x01vrecord";
    if (host.clip_type == ClipType::Record)      return "\x01record";
    return !host.source_id.empty() ? host.source_id : host.text;
}

static bool fx_video_host_type(ClipType t) {
    return clip_is_videolike_type(t) || t == ClipType::Background;
}

int fx_coupled_host(const AppState& state, int fx_ti, const Clip& fx_cl) {
    if (!fx_cl.fx_coupled || fx_cl.fx_host_sid.empty()) return -1;
    if (fx_ti < 0 || fx_ti >= (int)state.tracks.size()) return -1;
    const bool audio_kind = fx_brick_is_audio_kind(fx_cl);
    const auto& clips = state.tracks[fx_ti].clips;
    int best = -1; float best_ov = -1e9f;
    for (int ci = 0; ci < (int)clips.size(); ++ci) {
        const Clip& hc = clips[(size_t)ci];
        if (audio_kind ? !fx_audio_host_type(hc.clip_type)
                       : !fx_video_host_type(hc.clip_type)) continue;
        if (fx_host_fingerprint(hc) != fx_cl.fx_host_sid) continue;
        float ov = fminf(fx_cl.end, hc.end) - fmaxf(fx_cl.start, hc.start);
        if (ov > best_ov) { best_ov = ov; best = ci; }
    }
    return best;
}

void fx_coupling_tick(AppState& state) {
    for (int ti = 0; ti < (int)state.tracks.size(); ++ti) {
        auto& clips = state.tracks[ti].clips;
        for (auto& cl : clips) {
            if (!cl.fx_coupled ||
                !(fx_brick_is_video(cl) || fx_brick_is_audio_kind(cl))) continue;
            int host = fx_coupled_host(state, ti, cl);
            if (host < 0) {
                // Host deleted / moved off-track — the brick goes free.
                cl.fx_coupled = false;
                cl.fx_host_sid.clear();
                continue;
            }
            const Clip& hc = clips[(size_t)host];
            float old0 = cl.start, old1 = cl.end, oldlen = old1 - old0;
            float new0 = hc.start, new1 = hc.end, newlen = new1 - new0;

            // A length change means the host was TRIMMED: each windowed sub-effect
            // in the chain should hold its absolute timeline position instead of
            // rubber-banding with the brick. (A pure position change is a move —
            // the whole brick shifts, so the effects ride along untouched.)
            //   • always-on (0/0) spans the brick → follows the resize.
            //   • "until end" (rel_end<=0) keeps tracking the new end; only its
            //     start pins to absolute time.
            //   • windowed (rel_end>0) pins both edges, clamped to the new bounds.
            if (newlen > 1e-4f && fabsf(newlen - oldlen) > 1e-4f) {
                for (auto& se : cl.fx_chain) {
                    bool to_end     = (se.rel_end   <= 0.f);
                    bool from_start = (se.rel_start <= 0.001f);
                    if (from_start && to_end) continue;          // always-on
                    float abs0 = old0 + se.rel_start;
                    float rs   = fmaxf(0.f, fminf(abs0 - new0, newlen));
                    if (to_end) {
                        se.rel_start = rs;                       // rel_end stays 0
                    } else {
                        float abs1 = old0 + se.rel_end;
                        float re   = fmaxf(rs, fminf(abs1 - new0, newlen));
                        if (rs <= 0.001f && re >= newlen - 0.001f) { rs = 0.f; re = 0.f; }
                        se.rel_start = rs;
                        se.rel_end   = re;
                    }
                }
            }
            cl.start = new0;
            cl.end   = new1;
        }
    }

    // Keyframe time-base sync: a chain sub-effect is itself a Clip, and its
    // keyframes are stored relative to its own .start (eval_prop computes
    // t_local = playhead - clip.start). The kf_slider UI keys at the same
    // brick-relative time, so a sub-effect's start/end MUST track its parent
    // brick — then keys set in the panel and read during render share one time
    // base, and they ride along whenever the brick moves or welds. (Sub-effect
    // *windowing* still uses rel_start/rel_end; start/end is only the kf clock.)
    for (auto& tr : state.tracks)
        for (auto& cl : tr.clips)
            if (cl.clip_type == ClipType::MultiFX ||
                cl.clip_type == ClipType::AudioMultiFX)
                for (auto& se : cl.fx_chain) { se.start = cl.start; se.end = cl.end; }
}

// Glass: video FX bricks are glass iff coupled (explicit link); audio FX
// bricks keep the legacy same-track-overlap rule until the audio chain
// brick lands.
bool fx_clip_is_glass(const AppState& state, int fx_ti, const Clip& fx_cl) {
    if (fx_brick_is_video(fx_cl) || fx_cl.clip_type == ClipType::AudioMultiFX)
        return fx_cl.fx_coupled && fx_coupled_host(state, fx_ti, fx_cl) >= 0;
    if (fx_ti < 0 || fx_ti >= (int)state.tracks.size()) return false;
    for (auto& cl : state.tracks[fx_ti].clips) {
        if (cl.clip_type != ClipType::Video && cl.clip_type != ClipType::Audio &&
            cl.clip_type != ClipType::Background &&
            cl.clip_type != ClipType::VideoRecord) continue;
        if (fx_cl.start < cl.end && fx_cl.end > cl.start) return true;
    }
    return false;
}

int fx_glass_host_index(const AppState& state, int fx_ti, const Clip& fx_cl) {
    return fx_coupled_host(state, fx_ti, fx_cl);
}

// Helper: accumulate all sub-effects of a MultiFX brick that are active at time t.
static void accum_multifx_effects(EffectAccum& ea, CreativeFXAccum& ca,
                                  const AppState& state, const Clip& brick, float t) {
    float rel        = t - brick.start;
    float parent_dur = brick.end - brick.start;
    float beat_pulse = beat_pulse_at(state, brick.beat_src_track, brick.beat_src_clip, t, brick.beat_decay);
    for (auto& se : brick.fx_chain) {
        if (se.clip_type == ClipType::BodyFX) continue;  // handled by glass BodyFX pass
        float se_end = (se.rel_end <= 0.f) ? parent_dur : se.rel_end;
        if (rel < se.rel_start || rel >= se_end) continue;
        accum_effect_clip(ea, se, t);
        accum_creative_clip(ca, se, beat_pulse, t);
    }
}

// Global adjustment FX: clips on tracks above below_track_idx that are NOT glass.
EffectAccum collect_effects(const AppState& state, float t, int below_track_idx) {
    EffectAccum acc;
    for (int ti = 0; ti < below_track_idx && ti < (int)state.tracks.size(); ++ti) {
        for (auto& cl : state.tracks[ti].clips) {
            if (cl.clip_type == ClipType::MultiFX) {
                if (t < cl.start || t >= cl.end) continue;
                if (fx_clip_is_glass(state, ti, cl)) continue;
                CreativeFXAccum dummy;
                accum_multifx_effects(acc, dummy, state, cl, t);
                continue;
            }
            if (cl.clip_type != ClipType::Effect) continue;
            if (t < cl.start || t >= cl.end) continue;
            if (fx_clip_is_glass(state, ti, cl)) continue;
            accum_effect_clip(acc, cl, t);
        }
    }
    return acc;
}

// Glass adjustment FX: Effect/MultiFX clips on the same track as the video clip that overlap it.
EffectAccum collect_glass_effects(const AppState& state, float t, int video_track_idx) {
    EffectAccum acc;
    if (video_track_idx < 0 || video_track_idx >= (int)state.tracks.size()) return acc;
    for (auto& cl : state.tracks[video_track_idx].clips) {
        if (cl.clip_type == ClipType::MultiFX) {
            if (t < cl.start || t >= cl.end) continue;
            if (!fx_clip_is_glass(state, video_track_idx, cl)) continue;
            CreativeFXAccum dummy;
            accum_multifx_effects(acc, dummy, state, cl, t);
            continue;
        }
        if (cl.clip_type != ClipType::Effect) continue;
        if (t < cl.start || t >= cl.end) continue;
        if (!fx_clip_is_glass(state, video_track_idx, cl)) continue;
        accum_effect_clip(acc, cl, t);
    }
    // Per-clip grade from the video clip itself
    for (auto& cl : state.tracks[video_track_idx].clips) {
        if (cl.clip_type != ClipType::Video && cl.clip_type != ClipType::Background) continue;
        if (t < cl.start || t >= cl.end) continue;
        if (cl.grade_brightness != 0.f || cl.grade_contrast != 1.f ||
            cl.grade_saturation != 1.f || cl.grade_hue      != 0.f) {
            acc.brightness += cl.grade_brightness;
            acc.contrast   *= cl.grade_contrast;
            acc.saturation *= cl.grade_saturation;
            acc.hue        += cl.grade_hue;
            acc.any_color   = true;
        }
        break;
    }
    return acc;
}

CreativeFXAccum collect_creative_fx(const AppState& state, float t, int below_track_idx) {
    CreativeFXAccum acc;
    for (int ti = 0; ti < below_track_idx && ti < (int)state.tracks.size(); ++ti) {
        for (auto& cl : state.tracks[ti].clips) {
            if (cl.clip_type == ClipType::MultiFX) {
                if (t < cl.start || t >= cl.end) continue;
                if (fx_clip_is_glass(state, ti, cl)) continue;
                EffectAccum dummy;
                accum_multifx_effects(dummy, acc, state, cl, t);
                continue;
            }
            if (cl.clip_type != ClipType::Effect) continue;
            if (t < cl.start || t >= cl.end)       continue;
            if (fx_clip_is_glass(state, ti, cl))   continue;
            float _cl_beat_pulse = beat_pulse_at(state, cl.beat_src_track, cl.beat_src_clip, t, cl.beat_decay);
            accum_creative_clip(acc, cl, _cl_beat_pulse, t);
        }
    }
    return acc;
}

// Glass creative FX: Effect/MultiFX clips on the same track as the video clip that overlap it.
CreativeFXAccum collect_glass_fx(const AppState& state, float t, int video_track_idx) {
    CreativeFXAccum acc;
    if (video_track_idx < 0 || video_track_idx >= (int)state.tracks.size()) return acc;
    for (auto& cl : state.tracks[video_track_idx].clips) {
        if (cl.clip_type == ClipType::MultiFX) {
            if (t < cl.start || t >= cl.end) continue;
            if (!fx_clip_is_glass(state, video_track_idx, cl)) continue;
            EffectAccum dummy;
            accum_multifx_effects(dummy, acc, state, cl, t);
            continue;
        }
        if (cl.clip_type != ClipType::Effect) continue;
        if (t < cl.start || t >= cl.end)       continue;
        if (!fx_clip_is_glass(state, video_track_idx, cl)) continue;
        float _cl_beat_pulse = beat_pulse_at(state, cl.beat_src_track, cl.beat_src_clip, t, cl.beat_decay);
        accum_creative_clip(acc, cl, _cl_beat_pulse, t);
    }
    return acc;
}

// Single-track variants: collect global (non-glass) FX from exactly one track.
// Used by the scene compositor to apply per-track FX in z-order without bleeding
// effects across tracks.
EffectAccum collect_effects_for_track(const AppState& state, float t, int track_idx) {
    EffectAccum acc;
    if (track_idx < 0 || track_idx >= (int)state.tracks.size()) return acc;
    for (auto& cl : state.tracks[track_idx].clips) {
        if (cl.clip_type == ClipType::MultiFX) {
            if (t < cl.start || t >= cl.end) continue;
            if (fx_clip_is_glass(state, track_idx, cl)) continue;
            CreativeFXAccum dummy;
            accum_multifx_effects(acc, dummy, state, cl, t);
            continue;
        }
        if (cl.clip_type != ClipType::Effect) continue;
        if (t < cl.start || t >= cl.end) continue;
        if (fx_clip_is_glass(state, track_idx, cl)) continue;
        accum_effect_clip(acc, cl, t);
    }
    return acc;
}

CreativeFXAccum collect_creative_fx_for_track(const AppState& state, float t, int track_idx) {
    CreativeFXAccum acc;
    if (track_idx < 0 || track_idx >= (int)state.tracks.size()) return acc;
    for (auto& cl : state.tracks[track_idx].clips) {
        if (cl.clip_type == ClipType::MultiFX) {
            if (t < cl.start || t >= cl.end) continue;
            if (fx_clip_is_glass(state, track_idx, cl)) continue;
            EffectAccum dummy;
            accum_multifx_effects(dummy, acc, state, cl, t);
            continue;
        }
        if (cl.clip_type != ClipType::Effect) continue;
        if (t < cl.start || t >= cl.end)      continue;
        if (fx_clip_is_glass(state, track_idx, cl)) continue;
        float _cl_beat_pulse = beat_pulse_at(state, cl.beat_src_track, cl.beat_src_clip, t, cl.beat_decay);
        accum_creative_clip(acc, cl, _cl_beat_pulse, t);
    }
    return acc;
}

float beat_pulse_at(const AppState& state, int src_track, int src_clip, float t, float decay) {
    if (src_track < 0 || src_clip < 0) return 0.f;
    if (src_track >= (int)state.tracks.size()) return 0.f;
    const auto& track = state.tracks[src_track];
    if (src_clip >= (int)track.clips.size()) return 0.f;
    const auto& clip = track.clips[src_clip];
    if (clip.beats.empty()) return 0.f;
    float last_beat = -1.f;
    for (float b : clip.beats) {
        if (b <= t) last_beat = b;
        else break;
    }
    if (last_beat < 0.f) return 0.f;
    float elapsed = t - last_beat;
    float d = (decay > 0.001f) ? decay : 0.001f;
    return expf(-elapsed / d);
}

void app_init(AppState& state) {
    theme_apply();
    audio_init();
    render_init_fonts();
    fx_shader_init();
    state.user_presets = presets_load_user();
    std::string effects_dir = g_managed_dir + "/effects";
    runtime_fx_init(effects_dir);
    ipc_server_start();
    // Baseline snapshot: history_undo() can't step back past entry 0, so
    // without this the first edit of a session is never undoable.
    history_push(state, "Session start");
}

void app_frame(AppState& state) {
    // Coupled FX bricks track their host every frame — drags, trims, splits
    // and deletions all resolve here instead of in each edit path.
    fx_coupling_tick(state);

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::SetNextWindowBgAlpha(1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.f, 0.f});
    ImGui::Begin("##root", nullptr,
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize   |
        ImGuiWindowFlags_NoMove     |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_MenuBar
    );
    ImGui::PopStyleVar();  // WindowPadding

    // Transport loop: cycle the loop brace region (or the whole timeline when no
    // region is set) seamlessly while playing. The record bricks own the audio
    // loop region when active, so defer to them.
    float loop_lo = 0.f, loop_hi = state.duration;
    loop_region(state, loop_lo, loop_hi);
    bool transport_loop = state.loop_play && loop_hi > loop_lo &&
                          !recorder_active() && !vrecorder_active();
    if (transport_loop && state.playing) {
        // Drive the audio engine's sample-accurate wrap so the master clock
        // (and the playhead derived from it) loops with no seam. Re-set each
        // frame so a region/duration change mid-loop is picked up immediately.
        audio_set_loop(loop_lo, loop_hi);
    } else if (audio_loop_active() && !recorder_active() && !vrecorder_active()) {
        // Loop turned off / playback stopped → drop our transport loop region
        // (never a recorder's — those clear it themselves on stop).
        audio_clear_loop();
    }

    // Update playhead BEFORE rendering so the video frame shown this cycle
    // matches the audio position this cycle, not last cycle's.
    if (state.playing) {
        float pos;
        if (!audio_loading() && audio_is_playing()) {
            pos = audio_position() - audio_latency();
            if (pos < 0.f) pos = 0.f;
        } else {
            using namespace std::chrono;
            double elapsed = duration<double>(steady_clock::now() - state.play_start_wall).count();
            if (elapsed < 0.0) elapsed = 0.0;
            pos = state.play_start_pos + (float)elapsed;
        }
        if (transport_loop) {
            // The audio clock already wraps when sound is playing; this also
            // covers the silent (wall-clock) path, where pos grows unbounded.
            // Wrap at the region end → region start (playback before the region
            // plays in, then cycles — matches Ableton). Re-anchor the wall-clock
            // origin on wrap so drift can't accumulate.
            if (pos >= loop_hi) {
                pos = loop_lo + fmodf(pos - loop_lo, loop_hi - loop_lo);
                state.play_start_pos  = pos;
                state.play_start_wall = std::chrono::steady_clock::now();
                audio_seek(pos);
            }
            state.playhead = pos;
        } else {
            state.playhead = pos;
            // No end-of-project auto-stop while a loop region cycles (record
            // brick): the brick may extend past current content, and the wrap
            // keeps the playhead inside the loop anyway.
            if (!audio_loop_active() &&
                state.duration > 0.f && state.playhead >= state.duration) {
                // Park on the last real frame, not the exclusive end (which shows
                // nothing) — so playing to the end leaves the last frame on screen.
                state.playhead = last_playable_time(state);
                state.playing  = false;
                audio_pause();
                audio_seek(0.f);
            }
        }
    }
    // Single source of truth when paused: the playhead can never sit past the
    // last frame (replaces the old render-time nudge in canvas.cpp). Left alone
    // while a record-brick loop cycles past current content.
    if (!state.playing && !audio_loop_active() && state.duration > 0.f)
        state.playhead = fmaxf(0.f, fminf(state.playhead, last_playable_time(state)));

    // IPC-requested export: pick up on GL thread before ticking the render.
    if (state.export_request && !state.render.running) {
        state.export_request = false;
        if (!state.export_out_path.empty())
            state.out_mp4 = state.export_out_path;
        // sync gif path
        std::string mp4 = state.out_mp4;
        size_t dot = mp4.rfind('.');
        state.out_gif = (dot != std::string::npos ? mp4.substr(0, dot) : mp4) + ".gif";
        render_start_gl(state);
    }

    // Drive one export frame per app frame (GL calls must be on main thread).
    render_tick_gl(state);

    // Hot-reload custom effects and dispatch IPC messages.
    runtime_fx_poll(g_managed_dir + "/effects");
    ipc_server_poll(state);

    if (state.splash_timer > 0.f) {
        state.splash_timer -= io.DeltaTime;
        ui_splash(state);
    } else if (!state.models_ready && !state.models_skipped) {
        ui_setup(state);
    } else {
        ui_studio(state);
    }

    ImGui::End();
}

void app_shutdown(AppState& state) {
    g_shutdown.store(true);
    agent_shutdown();
    ipc_server_stop();
    vrecorder_shutdown();   // kill the camera-capture child so it isn't orphaned
    runtime_fx_shutdown();
    audio_shutdown();
    video_close();
    transcribe_cancel();
    fx_shader_shutdown();
    terminal_panel_shutdown();
    (void)state;
}
