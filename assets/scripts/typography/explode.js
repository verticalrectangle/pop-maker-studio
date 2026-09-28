// Explode In — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "explode", "name": "Explode In", "category": "Kinetic", "tagline": "Letters fly in from everywhere to place \u00b7 impact"};
const config = {"id": "explode", "tagline": "Letters fly in from everywhere to place \u00b7 impact", "category": "Kinetic", "font": "anton", "grouping": "Word", "custom_n": 1, "font_size": 0.15, "pause_gap": 0.2, "anim_stagger": 0.015, "sub_pos": 1, "max_words": 1, "anim_unit": 2, "all_caps": true, "style": "Explode", "n_fx": 0, "fx": [], "ts": {}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
