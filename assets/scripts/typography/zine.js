// Zine — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "zine", "name": "Zine", "category": "Editorial", "tagline": "Mixed case \u00b7 phrases \u00b7 lo-fi rotated energy"};
const config = {"id": "zine", "tagline": "Mixed case \u00b7 phrases \u00b7 lo-fi rotated energy", "category": "Editorial", "font": "archivoblack", "grouping": "Phrase", "custom_n": 3, "font_size": 0.09, "pause_gap": 0.35, "sub_pos": 1, "max_words": 5, "style": "Glitch", "n_fx": 0, "fx": [], "ts": {}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
