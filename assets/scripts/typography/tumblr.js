// Tumblr — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "tumblr", "name": "Tumblr", "category": "Aesthetic", "tagline": "Helvetica energy \u00b7 lowercase \u00b7 centered \u00b7 clean"};
const config = {"id": "tumblr", "tagline": "Helvetica energy \u00b7 lowercase \u00b7 centered \u00b7 clean", "category": "Aesthetic", "font": null, "grouping": "Phrase", "custom_n": 3, "font_size": 0.08, "sub_wrap_w": 0.8, "pause_gap": 0.4, "sub_pos": 1, "max_words": 6, "text_case": 2, "style": "None", "n_fx": 0, "fx": [], "ts": {}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
