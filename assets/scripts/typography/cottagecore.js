// Cottagecore — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "cottagecore", "name": "Cottagecore", "category": "Aesthetic", "tagline": "Warm cream \u00b7 serif \u00b7 gentle film grain"};
const config = {"id": "cottagecore", "tagline": "Warm cream \u00b7 serif \u00b7 gentle film grain", "category": "Aesthetic", "font": "ebgaramond", "grouping": "Line", "custom_n": 3, "font_size": 0.075, "sub_wrap_w": 0.8, "pause_gap": 0.6, "sub_pos": 1, "max_words": 6, "text_case": 2, "style": "None", "color": [0.96, 0.91, 0.78, 1.0], "n_fx": 1, "fx": [{"type": "FilmGrain", "beat": 0.0}], "ts": {}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
