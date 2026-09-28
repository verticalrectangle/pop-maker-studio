// True Type — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "truetype", "name": "True Type", "category": "Kinetic", "tagline": "Real char-by-char reveal \u00b7 terminal \u00b7 green"};
const config = {"id": "truetype", "tagline": "Real char-by-char reveal \u00b7 terminal \u00b7 green", "category": "Kinetic", "font": "vt323", "grouping": "CustomN", "custom_n": 5, "font_size": 0.08, "sub_pos_x": 0.08, "sub_pos_y": 0.85, "sub_wrap_w": 0.85, "pause_gap": 0.4, "anim_stagger": 0.035, "sub_pos": 0, "sub_anchor_h": 0, "max_words": 8, "anim_unit": 2, "style": "Typewriter", "color": [0.3, 1.0, 0.4, 1.0], "n_fx": 0, "fx": [], "ts": {"shadow_enabled": false}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
