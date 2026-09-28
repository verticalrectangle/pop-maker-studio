// Thin & Wide — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "thinwide", "name": "Thin & Wide", "category": "Editorial", "tagline": "Delicate tracked caps \u00b7 luxury \u00b7 slow"};
const config = {"id": "thinwide", "tagline": "Delicate tracked caps \u00b7 luxury \u00b7 slow", "category": "Editorial", "font": "cormorant", "grouping": "Phrase", "custom_n": 3, "font_size": 0.09, "sub_wrap_w": 0.85, "pause_gap": 0.6, "tracking": 0.2, "sub_pos": 1, "max_words": 4, "all_caps": true, "style": "None", "n_fx": 0, "fx": [], "ts": {"shadow_enabled": false}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
