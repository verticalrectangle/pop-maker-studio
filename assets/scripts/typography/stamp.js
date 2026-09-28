// Stamp — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "stamp", "name": "Stamp", "category": "Hype", "tagline": "Slams down rotated \u00b7 red \u00b7 approved energy"};
const config = {"id": "stamp", "tagline": "Slams down rotated \u00b7 red \u00b7 approved energy", "category": "Hype", "font": "bebasneue", "grouping": "Word", "custom_n": 1, "font_size": 0.16, "pause_gap": 0.12, "sub_pos": 1, "max_words": 1, "all_caps": true, "style": "Scale", "color": [0.92, 0.12, 0.12, 1.0], "n_fx": 0, "fx": [], "ts": {"stroke_enabled": true, "stroke_w": 2.0}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
