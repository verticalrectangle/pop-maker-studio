// Gravity — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "gravity", "name": "Gravity", "category": "Kinetic", "tagline": "Letters drop from above and bounce to the baseline"};
const config = {"id": "gravity", "tagline": "Letters drop from above and bounce to the baseline", "category": "Kinetic", "font": "bebasneue", "grouping": "Word", "custom_n": 1, "font_size": 0.17, "pause_gap": 0.2, "anim_stagger": 0.04, "sub_pos": 1, "max_words": 1, "anim_unit": 2, "all_caps": true, "style": "Gravity", "n_fx": 0, "fx": [], "ts": {}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
