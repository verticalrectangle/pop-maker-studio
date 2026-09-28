// Marker — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "marker", "name": "Marker", "category": "Script", "tagline": "Bold scrawl \u00b7 raw \u00b7 slight punk energy"};
const config = {"id": "marker", "tagline": "Bold scrawl \u00b7 raw \u00b7 slight punk energy", "category": "Script", "font": "permanentmarker", "grouping": "Phrase", "custom_n": 3, "font_size": 0.11, "sub_wrap_w": 0.8, "pause_gap": 0.4, "sub_pos": 1, "max_words": 5, "style": "None", "n_fx": 0, "fx": [], "ts": {}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
