// Headline — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "headline", "name": "Headline", "category": "Editorial", "tagline": "Massive single word \u00b7 fills frame \u00b7 high contrast"};
const config = {"id": "headline", "tagline": "Massive single word \u00b7 fills frame \u00b7 high contrast", "category": "Editorial", "font": "archivoblack", "grouping": "Word", "custom_n": 1, "font_size": 0.2, "sub_wrap_w": 0.92, "pause_gap": 0.12, "sub_pos": 1, "max_words": 1, "all_caps": true, "style": "Scale", "n_fx": 0, "fx": [], "ts": {}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
