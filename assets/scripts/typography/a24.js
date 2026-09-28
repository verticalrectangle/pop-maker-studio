// A24 — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "a24", "name": "A24", "category": "Aesthetic", "tagline": "Tiny centered serif \u00b7 slow fade \u00b7 lots of air"};
const config = {"id": "a24", "tagline": "Tiny centered serif \u00b7 slow fade \u00b7 lots of air", "category": "Aesthetic", "font": "instrumentserif", "grouping": "Phrase", "custom_n": 3, "font_size": 0.07, "sub_wrap_w": 0.7, "pause_gap": 0.6, "sub_pos": 1, "max_words": 5, "style": "None", "color": [0.97, 0.95, 0.9, 1.0], "n_fx": 0, "fx": [], "ts": {"shadow_enabled": false}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
