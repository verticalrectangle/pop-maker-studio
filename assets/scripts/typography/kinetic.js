// Kinetic — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "kinetic", "name": "Kinetic", "category": "Clean", "tagline": "Words slide in from left \u00b7 clean \u00b7 modern"};
const config = {"id": "kinetic", "tagline": "Words slide in from left \u00b7 clean \u00b7 modern", "category": "Clean", "font": "spacegrotesk", "grouping": "Phrase", "custom_n": 3, "font_size": 0.08, "pause_gap": 0.3, "sub_pos": 1, "max_words": 5, "style": "Slide", "n_fx": 0, "fx": [], "ts": {}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
