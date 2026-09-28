// Chrome 80s — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "chrome80s", "name": "Chrome 80s", "category": "Gradient", "tagline": "Silver-to-blue chrome gradient \u00b7 synthwave"};
const config = {"id": "chrome80s", "tagline": "Silver-to-blue chrome gradient \u00b7 synthwave", "category": "Gradient", "font": "bebasneue", "grouping": "CustomN", "custom_n": 2, "font_size": 0.16, "sub_wrap_w": 0.9, "pause_gap": 0.2, "sub_pos": 1, "max_words": 2, "grad_mode": 1, "all_caps": true, "style": "Scale", "color": [0.95, 0.95, 1.0, 1.0], "grad_col2": [0.3, 0.45, 0.9, 1.0], "n_fx": 0, "fx": [], "ts": {"stroke_enabled": true, "stroke_w": 2.0}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
