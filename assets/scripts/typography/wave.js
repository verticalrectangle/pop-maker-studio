// Wave — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "wave", "name": "Wave", "category": "Kinetic", "tagline": "Letters ride a continuous sine \u00b7 playful"};
const config = {"id": "wave", "tagline": "Letters ride a continuous sine \u00b7 playful", "category": "Kinetic", "font": "poppins", "grouping": "Phrase", "custom_n": 3, "font_size": 0.1, "sub_wrap_w": 0.85, "pause_gap": 0.4, "anim_stagger": 0.0, "sub_pos": 1, "max_words": 4, "anim_unit": 2, "style": "WaveText", "n_fx": 0, "fx": [], "ts": {}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
