// Ember — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "ember", "name": "Ember", "category": "Gradient", "tagline": "Fill glows orange-to-red like fire \u00b7 intense"};
const config = {"id": "ember", "tagline": "Fill glows orange-to-red like fire \u00b7 intense", "category": "Gradient", "font": "anton", "grouping": "Word", "custom_n": 1, "font_size": 0.17, "pause_gap": 0.15, "sub_pos": 1, "max_words": 1, "grad_mode": 1, "all_caps": true, "style": "Scale", "color": [1.0, 0.85, 0.3, 1.0], "grad_col2": [0.85, 0.12, 0.05, 1.0], "n_fx": 0, "fx": [], "ts": {"shadow_enabled": false, "glow_enabled": true, "glow_r": 10.0}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
