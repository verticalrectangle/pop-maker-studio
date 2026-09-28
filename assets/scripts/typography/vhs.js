// VHS Lyrics — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "vhs", "name": "VHS Lyrics", "category": "Retro", "tagline": "CRT mono \u00b7 bottom third \u00b7 VHS grain \u00b7 tracking"};
const config = {"id": "vhs", "tagline": "CRT mono \u00b7 bottom third \u00b7 VHS grain \u00b7 tracking", "category": "Retro", "font": "vt323", "grouping": "Line", "custom_n": 3, "font_size": 0.075, "sub_pos_y": 0.88, "pause_gap": 0.6, "sub_pos": 0, "max_words": 7, "text_case": 2, "style": "None", "n_fx": 1, "fx": [{"type": "VHS", "beat": 0.0}], "ts": {}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
