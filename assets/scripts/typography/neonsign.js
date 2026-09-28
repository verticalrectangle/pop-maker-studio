// Neon Sign — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "neonsign", "name": "Neon Sign", "category": "Retro", "tagline": "Cyan tube glow \u00b7 flickers on \u00b7 bar sign"};
const config = {"id": "neonsign", "tagline": "Cyan tube glow \u00b7 flickers on \u00b7 bar sign", "category": "Retro", "font": "monoton", "grouping": "Phrase", "custom_n": 2, "font_size": 0.11, "sub_wrap_w": 0.85, "pause_gap": 0.4, "sub_pos": 1, "max_words": 3, "all_caps": true, "style": "None", "color": [0.3, 0.95, 1.0, 1.0], "n_fx": 0, "fx": [], "ts": {"shadow_enabled": false, "glow_enabled": true, "glow_r": 14.0}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
