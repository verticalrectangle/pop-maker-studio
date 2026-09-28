// Word Pop — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "wordpop", "name": "Word Pop", "category": "Kinetic", "tagline": "Each word pops to size as it's sung \u00b7 modern lyric video"};
const config = {"id": "wordpop", "tagline": "Each word pops to size as it's sung \u00b7 modern lyric video", "category": "Kinetic", "font": "anton", "grouping": "Phrase", "custom_n": 3, "font_size": 0.11, "sub_wrap_w": 0.85, "pause_gap": 0.35, "anim_stagger": 0.05, "sub_pos": 1, "max_words": 5, "anim_unit": 1, "all_caps": true, "style": "Scale", "n_fx": 0, "fx": [], "ts": {}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
