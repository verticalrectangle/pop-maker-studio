// Slam Stack — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "slamstack", "name": "Slam Stack", "category": "Kinetic", "tagline": "Words stack and each slams in \u00b7 rap multi-line"};
const config = {"id": "slamstack", "tagline": "Words stack and each slams in \u00b7 rap multi-line", "category": "Kinetic", "font": "anton", "grouping": "CustomN", "custom_n": 3, "font_size": 0.11, "sub_pos_x": 0.06, "sub_wrap_w": 0.5, "pause_gap": 0.3, "anim_stagger": 0.08, "sub_pos": 1, "sub_anchor_h": 0, "max_words": 3, "anim_unit": 1, "all_caps": true, "style": "Scale", "n_fx": 0, "fx": [], "ts": {}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
