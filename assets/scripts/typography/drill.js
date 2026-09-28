// Drill — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "drill", "name": "Drill", "category": "Hype", "tagline": "3 words \u00b7 top third \u00b7 yellow on black \u00b7 aggressive"};
const config = {"id": "drill", "tagline": "3 words \u00b7 top third \u00b7 yellow on black \u00b7 aggressive", "category": "Hype", "font": "archivoblack", "grouping": "CustomN", "custom_n": 3, "font_size": 0.1, "sub_pos_y": 0.15, "pause_gap": 0.25, "sub_pos": 2, "max_words": 3, "all_caps": true, "style": "None", "color": [1.0, 0.95, 0.0, 1.0], "n_fx": 0, "fx": [], "ts": {}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
