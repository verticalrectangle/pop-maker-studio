// Neon Trace — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "neon_trace", "name": "Neon Trace", "category": "Karaoke", "tagline": "Active word's tube lights up as you sing \u00b7 neon sign"};
const config = {"id": "neon_trace", "tagline": "Active word's tube lights up as you sing \u00b7 neon sign", "category": "Karaoke", "font": "monoton", "grouping": "Line", "custom_n": 2, "font_size": 0.085, "sub_wrap_w": 0.85, "pause_gap": 0.5, "sub_pos": 1, "max_words": 5, "karaoke_mode": 1, "all_caps": true, "karaoke": true, "style": "None", "color": [0.4, 0.2, 0.4, 1.0], "karaoke_hi": [1.0, 0.3, 0.85, 1.0], "n_fx": 0, "fx": [], "ts": {"shadow_enabled": false, "glow_enabled": true, "glow_r": 10.0}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
