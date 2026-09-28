// Brush — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "brush", "name": "Brush", "category": "Script", "tagline": "Thick brush hand \u00b7 pops on the beat \u00b7 streetwear"};
const config = {"id": "brush", "tagline": "Thick brush hand \u00b7 pops on the beat \u00b7 streetwear", "category": "Script", "font": "caveat", "grouping": "Word", "custom_n": 1, "font_size": 0.16, "pause_gap": 0.15, "sub_pos": 1, "max_words": 1, "style": "Scale", "color": [0.95, 0.2, 0.2, 1.0], "n_fx": 0, "fx": [], "ts": {}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
