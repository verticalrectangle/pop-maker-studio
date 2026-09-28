// Indie 2012 — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "indie2012", "name": "Indie 2012", "category": "Aesthetic", "tagline": "Small \u00b7 muted \u00b7 left offset \u00b7 minimal"};
const config = {"id": "indie2012", "tagline": "Small \u00b7 muted \u00b7 left offset \u00b7 minimal", "category": "Aesthetic", "font": null, "grouping": "Line", "custom_n": 3, "font_size": 0.06, "sub_pos_x": 0.12, "sub_wrap_w": 0.75, "pause_gap": 0.5, "sub_pos": 1, "sub_anchor_h": 0, "max_words": 5, "text_case": 2, "style": "None", "color": [0.85, 0.82, 0.78, 1.0], "n_fx": 0, "fx": [], "ts": {}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
