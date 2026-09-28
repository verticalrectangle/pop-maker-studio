// Line Build — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "linebuild", "name": "Line Build", "category": "Kinetic", "tagline": "Word-by-word fade left to right \u00b7 clean caption build"};
const config = {"id": "linebuild", "tagline": "Word-by-word fade left to right \u00b7 clean caption build", "category": "Kinetic", "font": null, "grouping": "Phrase", "custom_n": 3, "font_size": 0.08, "sub_wrap_w": 0.8, "pause_gap": 0.35, "anim_stagger": 0.08, "sub_pos": 1, "max_words": 6, "anim_unit": 1, "style": "Typewriter", "n_fx": 0, "fx": [], "ts": {}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
