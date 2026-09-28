// Letterpress — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "letterpress", "name": "Letterpress", "category": "Editorial", "tagline": "Roman caps \u00b7 inset shadow \u00b7 cinematic stone"};
const config = {"id": "letterpress", "tagline": "Roman caps \u00b7 inset shadow \u00b7 cinematic stone", "category": "Editorial", "font": "cinzel", "grouping": "Phrase", "custom_n": 3, "font_size": 0.09, "sub_wrap_w": 0.85, "pause_gap": 0.6, "tracking": 0.06, "sub_pos": 1, "max_words": 5, "all_caps": true, "style": "None", "color": [0.88, 0.85, 0.8, 1.0], "n_fx": 0, "fx": [], "ts": {"shadow_enabled": true, "shadow_ox": -1.0, "shadow_oy": -1.0}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
