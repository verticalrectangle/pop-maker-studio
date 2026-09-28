// Subtitle — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "subtitleclean", "name": "Subtitle", "category": "Clean", "tagline": "Netflix-style bottom caption \u00b7 clean \u00b7 shadow"};
const config = {"id": "subtitleclean", "tagline": "Netflix-style bottom caption \u00b7 clean \u00b7 shadow", "category": "Clean", "font": null, "grouping": "Line", "custom_n": 3, "font_size": 0.05, "sub_pos_y": 0.9, "sub_wrap_w": 0.9, "pause_gap": 0.7, "sub_pos": 0, "max_words": 10, "style": "None", "n_fx": 0, "fx": [], "ts": {"shadow_enabled": true, "shadow_ox": 2.0, "shadow_oy": 2.0}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
