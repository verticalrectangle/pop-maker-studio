// Elastic — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "elastic", "name": "Elastic", "category": "Kinetic", "tagline": "Letters overshoot and wobble into place \u00b7 springy"};
const config = {"id": "elastic", "tagline": "Letters overshoot and wobble into place \u00b7 springy", "category": "Kinetic", "font": "sora", "grouping": "Word", "custom_n": 1, "font_size": 0.14, "pause_gap": 0.2, "anim_stagger": 0.035, "sub_pos": 1, "max_words": 1, "anim_unit": 2, "style": "Scale", "color": [0.7, 1.0, 0.85, 1.0], "n_fx": 0, "fx": [], "ts": {}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
