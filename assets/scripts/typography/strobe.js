// Strobe — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "strobe", "name": "Strobe", "category": "Hype", "tagline": "Flash but inverts every word \u00b7 ultra aggressive"};
const config = {"id": "strobe", "tagline": "Flash but inverts every word \u00b7 ultra aggressive", "category": "Hype", "font": "anton", "grouping": "Word", "custom_n": 1, "font_size": 0.18, "pause_gap": 0.08, "sub_pos": 1, "max_words": 1, "all_caps": true, "style": "None", "n_fx": 0, "fx": [], "ts": {}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
