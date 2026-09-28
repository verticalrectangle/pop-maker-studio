// Calligraphy — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "calligraphy", "name": "Calligraphy", "category": "Script", "tagline": "Formal hand \u00b7 large \u00b7 gold \u00b7 soft glow \u00b7 wedding"};
const config = {"id": "calligraphy", "tagline": "Formal hand \u00b7 large \u00b7 gold \u00b7 soft glow \u00b7 wedding", "category": "Script", "font": "allura", "grouping": "Phrase", "custom_n": 3, "font_size": 0.15, "sub_wrap_w": 0.8, "pause_gap": 0.6, "sub_pos": 1, "max_words": 4, "style": "None", "color": [1.0, 0.9, 0.55, 1.0], "n_fx": 0, "fx": [], "ts": {"shadow_enabled": false, "glow_enabled": true, "glow_r": 10.0}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
