// Duotone — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "duotone", "name": "Duotone", "category": "Gradient", "tagline": "Two-colour diagonal split fill \u00b7 poster"};
const config = {"id": "duotone", "tagline": "Two-colour diagonal split fill \u00b7 poster", "category": "Gradient", "font": "anton", "grouping": "Word", "custom_n": 1, "font_size": 0.18, "pause_gap": 0.15, "sub_pos": 1, "max_words": 1, "grad_mode": 2, "all_caps": true, "style": "Scale", "color": [1.0, 0.3, 0.6, 1.0], "grad_col2": [0.3, 0.5, 1.0, 1.0], "n_fx": 0, "fx": [], "ts": {"shadow_enabled": false}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
