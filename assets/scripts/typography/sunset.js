// Sunset — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "sunset", "name": "Sunset", "category": "Gradient", "tagline": "Warm orange-to-pink gradient fill \u00b7 slow \u00b7 warm pop"};
const config = {"id": "sunset", "tagline": "Warm orange-to-pink gradient fill \u00b7 slow \u00b7 warm pop", "category": "Gradient", "font": "poppins", "grouping": "Phrase", "custom_n": 3, "font_size": 0.11, "sub_wrap_w": 0.85, "pause_gap": 0.4, "sub_pos": 1, "max_words": 4, "grad_mode": 1, "style": "None", "color": [1.0, 0.75, 0.25, 1.0], "grad_col2": [1.0, 0.25, 0.55, 1.0], "n_fx": 0, "fx": [], "ts": {"shadow_enabled": false}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
