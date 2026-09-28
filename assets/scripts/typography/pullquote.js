// Pull Quote — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "pullquote", "name": "Pull Quote", "category": "Editorial", "tagline": "Italic serif \u00b7 feature-article energy"};
const config = {"id": "pullquote", "tagline": "Italic serif \u00b7 feature-article energy", "category": "Editorial", "font": "fraunces", "grouping": "Line", "custom_n": 3, "font_size": 0.085, "sub_wrap_w": 0.8, "pause_gap": 0.6, "sub_pos": 1, "max_words": 8, "style": "None", "color": [0.97, 0.96, 0.93, 1.0], "n_fx": 0, "fx": [], "ts": {"shadow_enabled": false}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
