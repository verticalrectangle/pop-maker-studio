// Flash — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "flash", "name": "Flash", "category": "Hype", "tagline": "One word \u00b7 full-frame \u00b7 cuts hard \u00b7 pure energy"};
const config = {"id": "flash", "tagline": "One word \u00b7 full-frame \u00b7 cuts hard \u00b7 pure energy", "category": "Hype", "font": "anton", "grouping": "Word", "custom_n": 1, "font_size": 0.18, "sub_wrap_w": 0.85, "pause_gap": 0.08, "sub_pos": 1, "max_words": 1, "all_caps": true, "style": "None", "n_fx": 0, "fx": [], "ts": {}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
