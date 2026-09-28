// Manifesto — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "manifesto", "name": "Manifesto", "category": "Editorial", "tagline": "Full left flush \u00b7 stacked \u00b7 uppercase \u00b7 maximum"};
const config = {"id": "manifesto", "tagline": "Full left flush \u00b7 stacked \u00b7 uppercase \u00b7 maximum", "category": "Editorial", "font": "archivoblack", "grouping": "Line", "custom_n": 3, "font_size": 0.11, "sub_pos_x": 0.05, "sub_wrap_w": 0.92, "pause_gap": 0.45, "sub_pos": 1, "sub_anchor_h": 0, "max_words": 5, "all_caps": true, "style": "None", "n_fx": 0, "fx": [], "ts": {}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
