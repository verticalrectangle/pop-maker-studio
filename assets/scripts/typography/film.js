// Film — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "film", "name": "Film", "category": "Aesthetic", "tagline": "Bottom third \u00b7 sentence \u00b7 classic subtitle"};
const config = {"id": "film", "tagline": "Bottom third \u00b7 sentence \u00b7 classic subtitle", "category": "Aesthetic", "font": null, "grouping": "Line", "custom_n": 3, "font_size": 0.055, "sub_pos_y": 0.88, "pause_gap": 0.7, "sub_pos": 0, "max_words": 8, "style": "None", "n_fx": 0, "fx": [], "ts": {}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
